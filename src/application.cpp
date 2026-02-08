
#include "application.h"
#include <random>
#include <boost/json.hpp>
#include <cmath>
#include <algorithm>
#include <pqxx/pqxx>

namespace app {

    namespace {

        std::string GenerateToken() {
            std::random_device rd;
            std::mt19937_64 gen1(rd());
            std::mt19937_64 gen2(rd());
            std::uniform_int_distribution<uint64_t> dist;

            uint64_t part1 = dist(gen1);
            uint64_t part2 = dist(gen2);

            std::stringstream ss;
            ss << std::hex << std::setfill('0')
                << std::setw(16) << part1
                << std::setw(16) << part2;
            return ss.str();
        }

    }

    void Application::InitializeCollisionDetectors() {
        for (const auto& map : game_.GetMaps()) {
            collision_detectors_[map.GetId()] =
                std::make_unique<model::CollisionDetector>(map);
        }
    }

    void Application::Tick(std::chrono::milliseconds delta) {
        double delta_seconds = delta.count() / 1000.0;
        UpdateGameState(delta_seconds);

        GenerateLootItems();
        CheckPlayerRetirement(delta);
    }

    void Application::UpdateGameState(double delta_time_seconds) {
        for (auto& dog : game_.GetDogs()) {
            auto start_pos = dog.GetPosition();
            MoveDog(dog, delta_time_seconds);
            auto end_pos = dog.GetPosition();

            ProcessDogCollisions(dog, start_pos, end_pos);
        }
    }

    void Application::MoveDog(model::Dog& dog, double delta_time) {
        if (dog.GetVelocity().vx == 0 && dog.GetVelocity().vy == 0) {
            return;
        }

        auto map_id = dog.GetMapId();
        auto it = collision_detectors_.find(map_id);
        if (it == collision_detectors_.end()) {
            return;
        }

        auto& detector = *it->second;
        auto current_pos = dog.GetPosition();
        auto velocity = dog.GetVelocity();

        auto movement_result = detector.CalculateMovement(current_pos, velocity, delta_time);

        dog.SetPosition(movement_result.new_position);

        if (movement_result.collision_occurred) {
            dog.SetVelocity({ 0.0, 0.0 });
        }
    }

    void Application::ProcessDogCollisions(model::Dog& dog,
        const model::Position& start_pos,
        const model::Position& end_pos) {
        const auto* map = FindMap(dog.GetMapId());
        if (!map) return;

        std::vector<CollisionEvent> events;

        for (const auto& loot : map->GetLootItems()) {
            auto collision_time = FindCollisionTime(start_pos, end_pos,
                loot.GetPosition(), 0.3);

            if (collision_time.has_value()) {
                events.push_back({
                    CollisionEvent::ITEM_PICKUP,
                    collision_time.value(),
                    dog.GetId(),
                    *loot.GetId(),
                    loot.GetType()
                    });
            }
        }

        for (const auto& office : map->GetOffices()) {
            auto office_pos = model::Position{
                static_cast<double>(office.GetPosition().x),
                static_cast<double>(office.GetPosition().y)
            };

            auto collision_time = FindCollisionTime(start_pos, end_pos,
                office_pos, 0.55);

            if (collision_time.has_value()) {
                events.push_back({
                    CollisionEvent::OFFICE_RETURN,
                    collision_time.value(),
                    dog.GetId(),
                    std::nullopt,
                    std::nullopt
                    });
            }
        }

        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.timestamp < b.timestamp; });

        for (const auto& event : events) {
            if (event.type == CollisionEvent::ITEM_PICKUP) {
                if (!dog.IsBagFull()) {
                    auto* item = const_cast<model::Map*>(map)->FindLootItem(model::LootItem::Id{ event.item_id.value() });
                    if (item) {
                        dog.AddToBag(*item);
                        const_cast<model::Map*>(map)->RemoveLootItem(item->GetId());
                    }
                }
                else {
                }
            }
            else if (event.type == CollisionEvent::OFFICE_RETURN) {
                for (const auto& bag_item : dog.GetBag()) {
                    dog.AddScore(bag_item.value);
                }
                dog.ClearBag();
            }
        }
    }

    std::optional<double> Application::FindCollisionTime(const model::Position& start_pos,
        const model::Position& end_pos,
        const model::Position& target_pos,
        double collision_distance) {
        double dx = end_pos.x - start_pos.x;
        double dy = end_pos.y - start_pos.y;
        double path_length = std::hypot(dx, dy);

        if (path_length < 1e-9) {
            double current_distance = std::hypot(target_pos.x - start_pos.x,
                target_pos.y - start_pos.y);
            return current_distance <= collision_distance ? 0.0 : std::nullopt;
        }

        double dir_x = dx / path_length;
        double dir_y = dy / path_length;

        double to_target_x = target_pos.x - start_pos.x;
        double to_target_y = target_pos.y - start_pos.y;

        double projection = to_target_x * dir_x + to_target_y * dir_y;

        double closest_x, closest_y;
        if (projection <= 0) {
            closest_x = start_pos.x;
            closest_y = start_pos.y;
        }
        else if (projection >= path_length) {
            closest_x = end_pos.x;
            closest_y = end_pos.y;
        }
        else {
            closest_x = start_pos.x + dir_x * projection;
            closest_y = start_pos.y + dir_y * projection;
        }

        double distance_to_path = std::hypot(target_pos.x - closest_x,
            target_pos.y - closest_y);

        if (distance_to_path > collision_distance) {
            return std::nullopt;
        }

        double distance_to_collision = projection - std::sqrt(collision_distance * collision_distance -
            distance_to_path * distance_to_path);

        if (distance_to_collision < 0 || distance_to_collision > path_length) {
            return std::nullopt;
        }

        return distance_to_collision / path_length;
    }

    const model::LootItem* Application::FindLootItem(const model::Map& map, int item_id) {
        for (const auto& loot : map.GetLootItems()) {
            if (*loot.GetId() == item_id) {
                return &loot;
            }
        }
        return nullptr;
    }

    void Application::GenerateLootItems() {
        static int next_loot_id = 0;

        for (auto& map : const_cast<model::Game&>(game_).GetMaps()) {
            if (map.GetLootItems().empty()) {
                for (int i = 0; i < 5; ++i) {
                    model::LootItem item(
                        model::LootItem::Id{ next_loot_id++ },
                        1, 
                        10.0,
                        { 10.0 + i * 5.0, 10.0 }
                    );
                    const_cast<model::Map&>(map).AddLootItem(std::move(item));
                }
            }
        }
    }

    std::optional<Application::JoinGameResult> Application::JoinGame(const std::string& user_name, const std::string& map_id) {
        model::Map::Id map_id_obj{ map_id };
        const auto* map = game_.FindMap(map_id_obj);
        if (!map) {
            return std::nullopt;
        }

        if (user_name.empty()) {
            return std::nullopt;
        }

        static uint32_t next_dog_id = 0;
        model::Dog::Id dog_id{ model::Dog::Id::value_type{next_dog_id++} };

        model::Position spawn_position;
        if (randomize_spawn_points_) {
            spawn_position = map->GetRandomDogPosition();
        }
        else {
            spawn_position = map->GetDefaultDogPosition();
        }

        model::Dog dog{ dog_id, user_name, map_id_obj, spawn_position };

        dog.SetBagCapacity(map->GetBagCapacity());

        game_.GetDogs().push_back(std::move(dog));

        static uint32_t next_player_id = 0;
        model::Player::Id player_id{ model::Player::Id::value_type{next_player_id++} };
        std::string token = GenerateToken();

        model::Player player{ player_id, user_name, dog_id, map_id_obj, token };
        game_.GetPlayers().push_back(std::move(player));

        auto& players = game_.GetPlayers();
        auto& dogs = game_.GetDogs();

        game_.GetTokenToPlayerIndex()[token] = players.size() - 1;
        game_.GetPlayerIdToIndex()[player_id] = players.size() - 1;
        game_.GetDogIdToIndex()[dog_id] = dogs.size() - 1;

        PlayerMetadata metadata;
        metadata.join_time = std::chrono::steady_clock::now();
        metadata.idle_start_time = std::nullopt;
        metadata.is_retired = false;
        player_metadata_[player_id] = metadata;

        return JoinGameResult{ token, player_id };
    }

    std::vector<const model::Player*> Application::GetPlayers(const std::string& auth_token) {
        auto player = FindPlayerByToken(auth_token);
        if (!player) {
            return {};
        }

        std::vector<const model::Player*> players_on_map;
        for (const auto& p : game_.GetPlayers()) {
            if (p.GetMapId() == player->GetMapId()) {
                players_on_map.push_back(&p);
            }
        }

        return players_on_map;
    }

    std::vector<const model::Player*> Application::GetGameState(const std::string& auth_token) {
        auto player = FindPlayerByToken(auth_token);
        if (!player) {
            return {};
        }

        std::vector<const model::Player*> players_on_map;
        for (const auto& p : game_.GetPlayers()) {
            if (p.GetMapId() == player->GetMapId()) {
                players_on_map.push_back(&p);
            }
        }

        return players_on_map;
    }

    bool Application::SetPlayerAction(const model::Player& player, const std::string& move) {
        auto* dog = FindDog(player.GetDogId());
        if (!dog) {
            return false;
        }

        const auto* map = FindMap(player.GetMapId());
        if (!map) {
            return false;
        }

        double speed = GetDogSpeedForMap(player.GetMapId());
        model::Velocity new_velocity{ 0.0, 0.0 };
        model::Direction new_direction = model::Direction::North;

        if (move == "L") {
            new_velocity = { -speed, 0.0 };
            new_direction = model::Direction::West;
        }
        else if (move == "R") {
            new_velocity = { speed, 0.0 };
            new_direction = model::Direction::East;
        }
        else if (move == "U") {
            new_velocity = { 0.0, -speed };
            new_direction = model::Direction::North;
        }
        else if (move == "D") {
            new_velocity = { 0.0, speed };
            new_direction = model::Direction::South;
        }
        else if (move == "") {
            new_velocity = { 0.0, 0.0 };
            new_direction = dog->GetDirection();
        }
        else {
            return false;
        }

        dog->SetVelocity(new_velocity);
        dog->SetDirection(new_direction);

        auto it = player_metadata_.find(player.GetId());
        if (it != player_metadata_.end() && !it->second.is_retired) {
            if (new_velocity.vx == 0.0 && new_velocity.vy == 0.0) {
                if (!it->second.idle_start_time.has_value()) {
                    it->second.idle_start_time = std::chrono::steady_clock::now();
                }
            } else {
                it->second.idle_start_time = std::nullopt;
            }
        }

        return true;
    }

    const model::Player* Application::FindPlayerByToken(const std::string& auth_token) {
        const auto& index = game_.GetTokenToPlayerIndex();
        auto it = index.find(auth_token);
        if (it == index.end()) {
            return nullptr;
        }

        const auto& players = game_.GetPlayers();
        if (it->second >= players.size()) {
            return nullptr;
        }

        return &players[it->second];
    }

    const model::Map* Application::FindMap(const model::Map::Id& id) const {
        return game_.FindMap(id);
    }

    model::Dog* Application::FindDog(const model::Dog::Id& id) {
        auto& index = game_.GetDogIdToIndex();
        auto it = index.find(id);
        if (it == index.end()) {
            return nullptr;
        }

        auto& dogs = game_.GetDogs();
        if (it->second >= dogs.size()) {
            return nullptr;
        }

        return &dogs[it->second];
    }

    double Application::GetDogSpeedForMap(const model::Map::Id& map_id) const {
        const auto* map = FindMap(map_id);
        if (!map) {
            return 1.0;
        }

        return map->GetDogSpeed();
    }

    void Application::CheckPlayerRetirement(std::chrono::milliseconds delta) {
        auto now = std::chrono::steady_clock::now();
        std::vector<model::Player::Id> players_to_retire;

        for (auto& [player_id, metadata] : player_metadata_) {
            if (metadata.is_retired) {
                continue;
            }

            auto* player = game_.FindPlayer(player_id);
            if (!player) {
                continue;
            }

            auto* dog = FindDog(player->GetDogId());
            if (!dog) {
                continue;
            }

            if (dog->GetVelocity().vx == 0.0 && dog->GetVelocity().vy == 0.0) {
                if (!metadata.idle_start_time.has_value()) {
                    metadata.idle_start_time = now;
                } else {
                    auto idle_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - metadata.idle_start_time.value()
                    );
                    double idle_seconds = idle_duration.count() / 1000.0;
                    if (idle_seconds >= dog_retirement_time_seconds_) {
                        players_to_retire.push_back(player_id);
                    }
                }
            } else {
                metadata.idle_start_time = std::nullopt;
            }
        }

        for (const auto& player_id : players_to_retire) {
            RetirePlayer(player_id);
        }
    }

    void Application::RetirePlayer(model::Player::Id player_id) {
        auto it = player_metadata_.find(player_id);
        if (it == player_metadata_.end() || it->second.is_retired) {
            return;
        }

        auto* player = game_.FindPlayer(player_id);
        if (!player) {
            return;
        }

        auto* dog = FindDog(player->GetDogId());
        if (!dog) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto play_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.join_time
        );
        double play_time_seconds = play_duration.count() / 1000.0;

        if (retirement_callback_) {
            retirement_callback_(player->GetName(), dog->GetScore(), play_time_seconds);
        }

        it->second.is_retired = true;

        auto& token_index = game_.GetTokenToPlayerIndex();
        token_index.erase(player->GetToken());

        auto& player_id_index = game_.GetPlayerIdToIndex();
        player_id_index.erase(player_id);

        auto& dog_id_index = game_.GetDogIdToIndex();
        dog_id_index.erase(dog->GetId());

        auto& players = game_.GetPlayers();
        players.erase(
            std::remove_if(players.begin(), players.end(),
                [player_id](const model::Player& p) { return p.GetId() == player_id; }),
            players.end()
        );

        auto& dogs = game_.GetDogs();
        dogs.erase(
            std::remove_if(dogs.begin(), dogs.end(),
                [dog_id = dog->GetId()](const model::Dog& d) { return d.GetId() == dog_id; }),
            dogs.end()
        );
    }

    namespace db {

        void Database::Initialize() {
            CreateTableIfNotExists();
            CreateIndexes();
        }

        void Database::CreateTableIfNotExists() {
            auto conn = pool_.GetConnection();
            pqxx::work tx{*conn};

            tx.exec(R"(
                CREATE TABLE IF NOT EXISTS retired_players (
                    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
                    name VARCHAR(255) NOT NULL,
                    score INTEGER NOT NULL,
                    play_time_ms BIGINT NOT NULL
                )
            )");

            tx.commit();
        }

        void Database::CreateIndexes() {
            auto conn = pool_.GetConnection();
            pqxx::work tx{*conn};

            try {
                auto result = tx.query_value<int>(
                    "SELECT COUNT(*) FROM pg_indexes WHERE schemaname = 'public' AND indexname = 'idx_retired_players_score_time_name'"
                );

                if (result == 0) {
                    tx.exec(R"(
                        CREATE INDEX idx_retired_players_score_time_name 
                        ON retired_players (score DESC, play_time_ms, name)
                    )");
                }
            } catch (const std::exception& e) {
            }

            tx.commit();
        }

        void Database::AddRetiredPlayer(const std::string& name, int score, double play_time_seconds) {
            auto conn = pool_.GetConnection();
            pqxx::work tx{*conn};

            int64_t play_time_ms = static_cast<int64_t>(play_time_seconds * 1000.0);

            tx.exec_params(
                "INSERT INTO retired_players (name, score, play_time_ms) VALUES ($1, $2, $3)",
                name, score, play_time_ms
            );

            tx.commit();
        }

        std::vector<RetiredPlayer> Database::GetRecords(int start, int max_items) {
            auto conn = pool_.GetConnection();
            pqxx::read_transaction tx{*conn};

            auto result = tx.exec_params(
                R"(
                    SELECT name, score, play_time_ms 
                    FROM retired_players 
                    ORDER BY score DESC, play_time_ms ASC, name ASC 
                    LIMIT $1 OFFSET $2
                )",
                max_items, start
            );

            std::vector<RetiredPlayer> records;
            records.reserve(result.size());

            for (const auto& row : result) {
                RetiredPlayer player;
                player.name = row[0].as<std::string>();
                player.score = row[1].as<int>();
                int64_t play_time_ms = row[2].as<int64_t>();
                player.play_time_seconds = play_time_ms / 1000.0;
                records.push_back(std::move(player));
            }

            return records;
        }

    }

}

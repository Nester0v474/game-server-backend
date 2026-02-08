
#pragma once
#include "model.h"
#include "collision_detector.h"
#include <chrono>
#include <unordered_map>
#include <memory>
#include <optional>
#include <functional>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace app {

    struct CollisionEvent {
        enum Type { ITEM_PICKUP, OFFICE_RETURN, ITEM_SKIP };
        Type type;
        double timestamp;
        model::Dog::Id dog_id;
        std::optional<int> item_id;
        std::optional<int> item_type;
    };

    struct PlayerMetadata {
        std::chrono::steady_clock::time_point join_time;
        std::optional<std::chrono::steady_clock::time_point> idle_start_time;
        bool is_retired = false;
    };

    class Application {
    public:
        Application(model::Game& game, bool randomize_spawn_points = false, double dog_retirement_time_seconds = 60.0)
            : game_(game)
            , randomize_spawn_points_(randomize_spawn_points)
            , dog_retirement_time_seconds_(dog_retirement_time_seconds) {
            InitializeCollisionDetectors();
        }

        struct JoinGameResult {
            std::string auth_token;
            model::Player::Id player_id;
        };

        std::optional<JoinGameResult> JoinGame(const std::string& user_name, const std::string& map_id);
        std::vector<const model::Player*> GetPlayers(const std::string& auth_token);
        std::vector<const model::Player*> GetGameState(const std::string& auth_token);
        bool SetPlayerAction(const model::Player& player, const std::string& move);

        void Tick(std::chrono::milliseconds delta);
        void UpdateGameState(double delta_time_seconds);
        const model::Player* FindPlayerByToken(const std::string& auth_token);
        bool ShouldRandomizeSpawnPoints() const { return randomize_spawn_points_; }
        const model::Map* FindMap(const model::Map::Id& id) const;
        model::Dog* FindDog(const model::Dog::Id& id);
        model::Game& GetGame() { return game_; }
        const model::Game& GetGame() const { return game_; }

        void SetRetirementCallback(std::function<void(const std::string& name, int score, double play_time_seconds)> callback) {
            retirement_callback_ = std::move(callback);
        }

    private:
        model::Game& game_;
        bool randomize_spawn_points_;
        double dog_retirement_time_seconds_;
        std::unordered_map<model::Map::Id, std::unique_ptr<model::CollisionDetector>> collision_detectors_;
        std::unordered_map<model::Player::Id, PlayerMetadata, util::TaggedHasher<model::Player::Id>> player_metadata_;
        std::function<void(const std::string& name, int score, double play_time_seconds)> retirement_callback_;

        void InitializeCollisionDetectors();
        void MoveDog(model::Dog& dog, double delta_time);
        double GetDogSpeedForMap(const model::Map::Id& map_id) const;

        void ProcessDogCollisions(model::Dog& dog, const model::Position& start_pos,
            const model::Position& end_pos);
        std::optional<double> FindCollisionTime(const model::Position& start_pos,
            const model::Position& end_pos,
            const model::Position& target_pos,
            double collision_distance);
        const model::LootItem* FindLootItem(const model::Map& map, int item_id);
        void GenerateLootItems();
        void CheckPlayerRetirement(std::chrono::milliseconds delta);
        void RetirePlayer(model::Player::Id player_id);
    };

    namespace db {

        struct RetiredPlayer {
            std::string name;
            int score;
            double play_time_seconds;
        };

        class ConnectionPool {
            using PoolType = ConnectionPool;
            using ConnectionPtr = std::shared_ptr<pqxx::connection>;

        public:
            class ConnectionWrapper {
            public:
                ConnectionWrapper(std::shared_ptr<pqxx::connection>&& conn, PoolType& pool) noexcept
                    : conn_{std::move(conn)}
                    , pool_{&pool} {
                }

                ConnectionWrapper(const ConnectionWrapper&) = delete;
                ConnectionWrapper& operator=(const ConnectionWrapper&) = delete;
                ConnectionWrapper(ConnectionWrapper&&) = default;
                ConnectionWrapper& operator=(ConnectionWrapper&&) = default;

                pqxx::connection& operator*() const& noexcept {
                    return *conn_;
                }

                pqxx::connection& operator*() const&& = delete;

                pqxx::connection* operator->() const& noexcept {
                    return conn_.get();
                }

                ~ConnectionWrapper() {
                    if (conn_) {
                        pool_->ReturnConnection(std::move(conn_));
                    }
                }

            private:
                std::shared_ptr<pqxx::connection> conn_;
                PoolType* pool_;
            };

            template <typename ConnectionFactory>
            ConnectionPool(size_t capacity, ConnectionFactory&& connection_factory) {
                pool_.reserve(capacity);
                for (size_t i = 0; i < capacity; ++i) {
                    pool_.emplace_back(connection_factory());
                }
            }

            ConnectionWrapper GetConnection() {
                std::unique_lock lock{mutex_};
                cond_var_.wait(lock, [this] {
                    return used_connections_ < pool_.size();
                });
                return {std::move(pool_[used_connections_++]), *this};
            }

        private:
            void ReturnConnection(ConnectionPtr&& conn) {
                {
                    std::lock_guard lock{mutex_};
                    assert(used_connections_ != 0);
                    pool_[--used_connections_] = std::move(conn);
                }
                cond_var_.notify_one();
            }

            std::mutex mutex_;
            std::condition_variable cond_var_;
            std::vector<ConnectionPtr> pool_;
            size_t used_connections_ = 0;
        };

        class Database {
        public:
            Database(ConnectionPool& pool) : pool_(pool) {}

            void Initialize();
            void AddRetiredPlayer(const std::string& name, int score, double play_time_seconds);
            std::vector<RetiredPlayer> GetRecords(int start = 0, int max_items = 100);

        private:
            ConnectionPool& pool_;
            void CreateTableIfNotExists();
            void CreateIndexes();
        };

    }

}

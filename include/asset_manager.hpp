// DarkHouse — digital asset management (DAM) catalog.
//
// The catalog is a single SQLite database in WAL mode (schema/catalog.sql).
// Imports run on a small worker pool. Hashing and metadata extraction run in
// parallel, and only the final INSERT is serialized on the writer connection.
// Queries use a separate read-only connection, so browsing the library never
// waits for an import to finish.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;

namespace darkhouse {

enum class ColorLabel : std::int32_t { NONE = 0, RED = 1, YELLOW = 2, GREEN = 3, BLUE = 4, PURPLE = 5 };

enum class AssetFlag : std::int32_t { REJECTED = -1, UNFLAGGED = 0, PICKED = 1 };

// Capture metadata. Every field is optional because RAW and EXIF coverage
// varies a lot between camera makers; nullopt maps to NULL in the catalog.
struct AssetMetadata {
    std::optional<std::string> cameraMake;
    std::optional<std::string> cameraModel;
    std::optional<std::string> lens;
    std::optional<std::int32_t> iso;
    std::optional<double> focalLength;   // millimetres
    std::optional<double> aperture;      // f-number
    std::optional<double> shutterSpeed;  // seconds
    std::optional<double> gpsLatitude;   // decimal degrees, positive = north
    std::optional<double> gpsLongitude;  // decimal degrees, positive = east
};

struct AssetRecord {
    std::string id;                            // UUIDv4, stable for the life of the catalog
    std::string filePath;
    std::string fileName;
    std::string fileHash;                      // "xxh64:<16 hex digits>"
    std::optional<std::int64_t> dateCaptured;  // Unix seconds (see schema/catalog.sql)
    std::int64_t dateImported = 0;             // Unix seconds, UTC
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t rating = 0;                   // 0..5 stars
    ColorLabel colorLabel = ColorLabel::NONE;
    AssetFlag flag = AssetFlag::UNFLAGGED;
    AssetMetadata metadata;
};

// One persisted step of an asset's non-destructive develop stack.
struct EditNodeRecord {
    std::int32_t nodeIndex = 0;
    std::string nodeType;                      // resolved by createComputeNode()
    std::vector<std::byte> serializedParams;   // passed to ComputeNode::updateUniforms()
};

// A value bound to a "?" placeholder in a queryAssets() filter.
struct QueryParam {
    QueryParam() = default;  // SQL NULL
    QueryParam(int v) : value(std::int64_t{v}) {}
    QueryParam(std::int64_t v) : value(v) {}
    QueryParam(double v) : value(v) {}
    QueryParam(std::string v) : value(std::move(v)) {}
    QueryParam(const char* v) : value(std::string(v)) {}

    std::variant<std::monostate, std::int64_t, double, std::string> value;
};

class AssetManager {
public:
    AssetManager();
    // Waits for in-flight imports to finish. Imports that are queued but have not
    // started are dropped, and their futures throw std::future_error (broken_promise).
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Opens the catalog file (creating it if needed), applies the embedded
    // schema, confirms WAL journaling and starts the import workers.
    // Throws std::runtime_error on failure.
    void initializeCatalog(const std::string& catalogPath);
    [[nodiscard]] bool isOpen() const noexcept;

    // Queues a file for import. The future yields the catalog record or rethrows
    // the I/O or database error. Importing content that is already in the catalog
    // (same file hash) is idempotent and yields the existing record.
    [[nodiscard]] std::future<AssetRecord> importFile(const std::string& absolutePath);

    // Runs:  SELECT ... FROM assets AS a LEFT JOIN metadata AS m ... WHERE (<sqlFilter>)
    //
    // `sqlFilter` is a boolean expression over the aliases `a` and `m`, for example
    // "a.rating >= ? AND m.iso <= ?". Pass values through `params`; never splice
    // untrusted text into the filter. The filter runs on a read-only connection
    // with an authorizer that allows only reads, so it cannot change the
    // catalog. Only one statement is accepted. Results come back newest capture
    // first, and an empty filter returns every asset.
    [[nodiscard]] std::vector<AssetRecord> queryAssets(const std::string& sqlFilter,
                                                       const std::vector<QueryParam>& params = {}) const;

    [[nodiscard]] std::optional<AssetRecord> getAsset(const std::string& assetId) const;

    // Each returns false when no asset has `assetId`.
    // updateAssetRating throws std::invalid_argument for ratings outside 0..5.
    bool updateAssetRating(const std::string& assetId, int rating);
    bool updateAssetFlag(const std::string& assetId, AssetFlag flag);
    bool updateAssetColorLabel(const std::string& assetId, ColorLabel label);

    // Replaces the asset's whole develop stack in one transaction.
    void saveEditStack(const std::string& assetId, const std::vector<EditNodeRecord>& nodes);
    // Returns the develop stack ordered by node_index.
    [[nodiscard]] std::vector<EditNodeRecord> loadEditStack(const std::string& assetId) const;

private:
    AssetRecord importFileBlocking(const std::string& absolutePath);
    std::optional<AssetRecord> findByHashLocked(const std::string& fileHash);
    bool updateIntegerColumn(const char* sql, const std::string& assetId, std::int64_t value);
    void workerLoop();
    void stopWorkers() noexcept;
    void closeDatabases() noexcept;
    void requireOpen() const;

    sqlite3* writeDb_ = nullptr;  // guarded by writeMutex_
    sqlite3* readDb_ = nullptr;   // guarded by readMutex_; opened read-only with an authorizer
    std::mutex writeMutex_;
    mutable std::mutex readMutex_;

    std::vector<std::thread> workers_;
    std::deque<std::packaged_task<AssetRecord()>> importQueue_;  // guarded by queueMutex_
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    bool stopping_ = false;  // guarded by queueMutex_
};

}  // namespace darkhouse

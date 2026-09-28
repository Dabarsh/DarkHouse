-- =============================================================================
-- DarkHouse catalog schema
--
-- Single source of truth for the DAM catalog. CMake embeds this file into the
-- executable (generated/catalog_schema.hpp) and AssetManager::initializeCatalog()
-- runs it on every open, so every statement must stay idempotent.
--
-- It also works standalone:  sqlite3 my_catalog.sqlite < schema/catalog.sql
--
-- Compatibility: SQLite >= 3.31. Avoids STRICT tables and unixepoch() so older
-- sqlite3 builds (e.g. the one in the macOS SDK) can still open the catalog.
-- =============================================================================

-- WAL lets the UI keep reading while the importer writes. journal_mode is stored
-- in the database file; foreign_keys is per connection and must be set outside
-- a transaction, so both pragmas come before BEGIN.
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;   -- with WAL: durable at checkpoint and never corrupts
PRAGMA foreign_keys = ON;

BEGIN;

-- -----------------------------------------------------------------------------
-- assets: one row per unique image. Source files are never modified.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS assets (
    id             TEXT    PRIMARY KEY NOT NULL,          -- UUIDv4; stays the same when the file moves
    file_path      TEXT    NOT NULL,                      -- absolute path at import time
    file_name      TEXT    NOT NULL,
    file_hash      TEXT    NOT NULL UNIQUE,               -- "xxh64:<hex>" content fingerprint; dedupes re-imports
    date_captured  INTEGER,                               -- Unix seconds from EXIF DateTimeOriginal (UTC when
                                                          -- OffsetTimeOriginal exists, else camera wall clock); NULL = unknown
    date_imported  INTEGER NOT NULL,                      -- Unix seconds, UTC
    width          INTEGER NOT NULL DEFAULT 0 CHECK (width  >= 0),
    height         INTEGER NOT NULL DEFAULT 0 CHECK (height >= 0),
    rating         INTEGER NOT NULL DEFAULT 0 CHECK (rating BETWEEN 0 AND 5),
    color_label    INTEGER NOT NULL DEFAULT 0 CHECK (color_label BETWEEN 0 AND 5),
                                                          -- 0 none, 1 red, 2 yellow, 3 green, 4 blue, 5 purple
    flag           INTEGER NOT NULL DEFAULT 0 CHECK (flag IN (-1, 0, 1))
                                                          -- -1 rejected, 0 unflagged, 1 picked
);

-- -----------------------------------------------------------------------------
-- metadata: capture data (1:1 with assets). NULL means the file did not record
-- the value; RAW and EXIF coverage varies a lot between camera makers.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS metadata (
    asset_id       TEXT PRIMARY KEY NOT NULL
                        REFERENCES assets(id) ON DELETE CASCADE,
    camera_make    TEXT,
    camera_model   TEXT,
    lens           TEXT,
    iso            INTEGER CHECK (iso IS NULL OR iso > 0),
    focal_length   REAL    CHECK (focal_length  IS NULL OR focal_length  > 0),   -- millimetres
    aperture       REAL    CHECK (aperture      IS NULL OR aperture      > 0),   -- f-number
    shutter_speed  REAL    CHECK (shutter_speed IS NULL OR shutter_speed > 0),   -- seconds (1/250 s = 0.004)
    gps_latitude   REAL    CHECK (gps_latitude  IS NULL OR gps_latitude  BETWEEN  -90 AND  90),
    gps_longitude  REAL    CHECK (gps_longitude IS NULL OR gps_longitude BETWEEN -180 AND 180)
);

-- -----------------------------------------------------------------------------
-- edit_nodes: an asset's non-destructive develop stack, evaluated in
-- node_index order. node_type selects the GPU ComputeNode (createComputeNode())
-- and serialized_params is passed as-is to ComputeNode::updateUniforms().
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS edit_nodes (
    asset_id           TEXT    NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    node_index         INTEGER NOT NULL CHECK (node_index >= 0),
    node_type          TEXT    NOT NULL,
    serialized_params  BLOB    NOT NULL,
    PRIMARY KEY (asset_id, node_index)
) WITHOUT ROWID;

-- -----------------------------------------------------------------------------
-- Indexes for the catalog grid's most common sorts and filters.
-- -----------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_assets_date_captured ON assets(date_captured);
CREATE INDEX IF NOT EXISTS idx_assets_rating        ON assets(rating);
CREATE INDEX IF NOT EXISTS idx_metadata_iso         ON metadata(iso);

PRAGMA user_version = 1;   -- bump alongside a migration when this schema changes

COMMIT;

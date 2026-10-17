/// @file history_logic.h
/// @brief Pure (host-testable) parts of the per-head history log: the fixed
/// 16-byte sample record, its field encodings, the bucketing that thins a long
/// window down to a chartable number of points, and the JSON row format.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace history {

constexpr uint32_t kIntervalS   = 30;      ///< one sample every 30 s
constexpr uint32_t kRetainDays  = 14;      ///< full days kept on flash
constexpr uint32_t kMaxPoints   = 1440;    ///< most rows one query returns
constexpr uint32_t kMinWindowS  = 3600;
constexpr uint32_t kMaxWindowS  = kRetainDays * 86400;
constexpr int16_t  kNoTemp      = INT16_MIN;  ///< "no reading" for a temperature
constexpr uint8_t  kNoFan       = 0xFF;

/// Mode codes (settings mode, not what the compressor is doing).
enum ModeCode : uint8_t {
    kModeNone = 0, kModeHeat, kModeCool, kModeDry, kModeFan, kModeAuto,
};

/// Flag bits.
enum FlagBit : uint8_t {
    kFlagConnected = 1 << 0,  ///< CN105 link up
    kFlagPower     = 1 << 1,  ///< on, as the user sees it (parked counts as on)
    kFlagOperating = 1 << 2,  ///< this head is calling (compressor serving it)
    kFlagDefrost   = 1 << 3,  ///< outdoor unit defrosting
    kFlagParked    = 1 << 4,  ///< HeatLink has it parked off (setpoint / slice)
};

/// One sample. Stored on flash verbatim, so the layout is fixed.
struct Record {
    uint32_t t       = 0;        ///< unix seconds, a multiple of kIntervalS
    int16_t  room    = kNoTemp;  ///< unit's room temperature, deci-°C
    int16_t  target  = kNoTemp;  ///< setpoint, deci-°C
    int16_t  outdoor = kNoTemp;  ///< outdoor temperature, deci-°C
    int16_t  sensor  = kNoTemp;  ///< room sensor, deci-°C (reserved)
    uint8_t  freq    = 0;        ///< compressor frequency (outdoor unit wide)
    uint8_t  mode    = kModeNone;
    uint8_t  flags   = 0;
    uint8_t  fan     = kNoFan;   ///< 0 AUTO, 1 QUIET, 2..5 = speeds 1..4
};
static_assert(sizeof(Record) == 16, "history record layout is stored on flash");

uint8_t mode_code(const std::string& mode);  ///< "HEAT" → kModeHeat, …
uint8_t fan_code(const std::string& fan);    ///< "AUTO" → 0, "QUIET" → 1, "1" → 2, …
int16_t to_deci(float c);                    ///< °C → rounded deci-°C

/// Bucket length for a window: kIntervalS, or the smallest multiple of it that
/// keeps the window within kMaxPoints rows.
uint32_t bucket_for(uint32_t window_s);

/// Clamp a requested window to [kMinWindowS, kMaxWindowS] (0 → 24 h).
uint32_t clamp_window(uint32_t window_s);

/// Folds time-ordered records into fixed buckets aligned to multiples of
/// @c bucket_s. Temperatures and frequency are averaged over the bucket's
/// readings; mode and fan are the last sample's; defrost is set if any sample
/// had it, the other flags if at least half did. A record older than the
/// current bucket (a backward clock step) is dropped.
class Bucketer {
public:
    explicit Bucketer(uint32_t bucket_s) : bucket_s_(bucket_s ? bucket_s : kIntervalS) {}
    /// Feed one record. Returns true and fills @p out when it closed a bucket.
    bool add(const Record& r, Record& out);
    /// Close the final bucket. Returns true and fills @p out if one was open.
    bool finish(Record& out);

private:
    void reset(uint32_t start);
    void accumulate(const Record& r);
    Record close() const;

    uint32_t bucket_s_;
    bool     open_ = false;
    uint32_t start_ = 0;
    uint32_t n_ = 0;
    int32_t  sum_[4] = {0, 0, 0, 0};
    uint32_t cnt_[4] = {0, 0, 0, 0};
    uint32_t freq_sum_ = 0;
    uint32_t flag_cnt_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t  mode_ = kModeNone;
    uint8_t  fan_  = kNoFan;
};

/// Write one record as a JSON array row:
///   [t, room, target, outdoor, sensor, freq, mode, flags, fan]
/// temperatures in deci-°C, null when absent (fan null when unknown). Returns
/// the length written (snprintf semantics).
int format_row(const Record& r, char* buf, size_t n);

/// Column names matching format_row, as a JSON array literal.
extern const char* const kFieldsJson;

}  // namespace history

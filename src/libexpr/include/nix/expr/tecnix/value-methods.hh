#pragma once

///@file

inline Value::Value(const Value & v)
{
    Value::Storage::operator=(v);
}

inline Value::Value(Value && v) noexcept
{
    Value::Storage::operator=(v);
}

inline Value & Value::operator=(const Value & v)
{
    Value::Storage::operator=(v);
    return *this;
}

inline Value & Value::operator=(Value && v) noexcept
{
    Value::Storage::operator=(v);
    return *this;
}

inline uint32_t Value::trackedSourceAccessSet() const noexcept
{
    return tecnixValueLabelLoad(this, std::memory_order_acquire);
}

inline void Value::setTrackedSourceAccessSet(uint32_t accessSet) noexcept
{
    tecnixValueLabelStore(this, accessSet, std::memory_order_release);
}

inline void Value::clearTrackedSourceAccessSet() noexcept
{
    tecnixValueLabelClear(this);
}

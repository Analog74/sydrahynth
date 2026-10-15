#include "i2c.h"

namespace hydra {

void I2cBus::release() {
    if (active_)
        active_->stop();
    active_ = nullptr;
}

bool I2cBus::update(bool scl, bool sda) {
    const bool sclRose = scl && !prevScl_, sclFell = !scl && prevScl_;
    const bool sdaChangedWhileHigh = scl && prevScl_ && sda != prevSda_;
    prevScl_ = scl;
    prevSda_ = sda;

    if (sdaChangedWhileHigh) {
        release();
        sdaOut_ = true;
        if (!sda) {  // START or repeated START
            phase_ = Phase::Address;
            bitCount_ = 0;
            shift_ = 0;
            masterNacked_ = false;
        } else {  // STOP
            phase_ = Phase::Idle;
        }
        return sdaOut_;
    }
    if (phase_ == Phase::Idle)
        return sdaOut_;

    const bool slaveTransmits = phase_ == Phase::Data && reading_;
    const bool line = sda && sdaOut_;
    if (sclRose) {
        if (bitCount_ < 8) {
            if (!slaveTransmits)
                shift_ = static_cast<std::uint8_t>(shift_ << 1 | line);
            ++bitCount_;
        } else if (bitCount_ == 8) {  // acknowledge clock
            if (slaveTransmits && line)
                masterNacked_ = true;  // master wants no more bytes
            bitCount_ = 9;
        }
    } else if (sclFell) {
        if (bitCount_ == 8) {
            if (slaveTransmits)
                sdaOut_ = true;  // let the master acknowledge
            else
                finishByte();
        } else if (bitCount_ == 9) {
            bitCount_ = 0;
            shift_ = 0;
            sdaOut_ = true;
            if (phase_ == Phase::Data && reading_ && !masterNacked_) {
                readByte_ = active_ ? active_->read() : 0xFF;
                sdaOut_ = readByte_ >> 7 & 1;
            }
        } else if (slaveTransmits && !masterNacked_ && bitCount_ >= 1 && bitCount_ <= 7) {
            sdaOut_ = readByte_ >> (7 - bitCount_) & 1;
        }
    }
    return sdaOut_;
}

void I2cBus::finishByte() {
    if (phase_ == Phase::Address) {
        const std::uint8_t address = shift_ >> 1;
        reading_ = shift_ & 1;
        const auto it = slaves_.find(address);
        active_ = it == slaves_.end() ? nullptr : it->second;
        if (active_)
            active_->start(reading_);
        else
            ++nackedAddresses_;
        phase_ = Phase::Data;
        sdaOut_ = active_ == nullptr;  // ACK pulls SDA low
        return;
    }
    sdaOut_ = !(active_ && active_->write(shift_));
}

}  // namespace hydra

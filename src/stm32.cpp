#include "stm32.h"

#include <algorithm>

namespace hydra::stm32 {

// --- RCC ----------------------------------------------------------------------

std::uint32_t Rcc::read(std::uint32_t offset, int) {
    const std::uint32_t index = offset >> 2;
    if (index >= regs_.size())
        return 0;
    const std::uint32_t v = regs_[index];
    switch (offset & ~3u) {
    case 0x00: return v | ((v << 1) & 0x02020002);  // HSI/HSE/PLL ready once on
    case 0x04: return (v & ~0xCu) | (v & 3) << 2;     // SWS = SW
    case 0x20: return v | ((v << 1) & 2);             // LSERDY
    case 0x24: return v | ((v << 1) & 2) | 0x0C000000;  // LSIRDY, pin and power-on reset flags
    default: return v;
    }
}

void Rcc::write(std::uint32_t offset, std::uint32_t value, int) {
    if ((offset >> 2) < regs_.size())
        regs_[offset >> 2] = value;
}

// --- GPIO ---------------------------------------------------------------------

std::uint16_t Gpio::levels() const {
    std::uint16_t levels = 0;
    for (int pin = 0; pin < 16; ++pin) {
        const std::uint32_t config = (pin < 8 ? crl_ : crh_) >> (4 * (pin & 7)) & 0xF;
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << pin);
        const bool driven = driven_ & bit, external = drivenLevels_ & bit, out = odr_ & bit;
        const bool openDrain = config & 4;
        bool level;
        if (config & 3) {  // output
            if (config & 8)                     // alternate function
                level = openDrain ? (!driven || external) : true;
            else
                level = openDrain ? out && (!driven || external) : out;
        } else if (driven) {
            level = external;
        } else {
            level = config == 8 && out;  // pull-up (ODR 1) / pull-down; floating reads 0
        }
        if (level)
            levels |= bit;
    }
    return levels;
}

void Gpio::update(std::uint16_t before) {
    const std::uint16_t after = levels();
    if (after != before)
        for (const auto& listener : onChange)
            listener(before, after);
}

std::uint32_t Gpio::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return crl_;
    case 0x04: return crh_;
    case 0x08: return levels();
    case 0x0C: return odr_;
    case 0x18: return lckr_;
    default: return 0;
    }
}

void Gpio::write(std::uint32_t offset, std::uint32_t value, int) {
    const std::uint16_t before = levels(), odrBefore = odr_;
    switch (offset & ~3u) {
    case 0x00: crl_ = value; break;
    case 0x04: crh_ = value; break;
    case 0x0C: odr_ = static_cast<std::uint16_t>(value); break;
    case 0x10: odr_ = static_cast<std::uint16_t>((odr_ & ~(value >> 16)) | (value & 0xFFFF)); break;  // BSRR
    case 0x14: odr_ = static_cast<std::uint16_t>(odr_ & ~value); break;                                // BRR
    case 0x18: lckr_ = value; break;
    default: return;
    }
    if (odr_ != odrBefore && onOutputChange)
        onOutputChange(odrBefore, odr_);
    update(before);
}

void Gpio::drive(std::uint16_t mask, bool high) {
    const std::uint16_t before = levels();
    driven_ |= mask;
    drivenLevels_ = high ? drivenLevels_ | mask : drivenLevels_ & ~mask;
    update(before);
}

void Gpio::release(std::uint16_t mask) {
    const std::uint16_t before = levels();
    driven_ &= ~mask;
    update(before);
}

// --- EXTI / AFIO ----------------------------------------------------------------

void Exti::raise(std::uint32_t lines) {
    pr_ |= lines;
    for (int line = 0; line < 16; ++line)
        if ((lines & imr_) >> line & 1)
            core_.setIrqPending(line < 5 ? 6 + line : line < 10 ? 23 : 40);
}

void Exti::pinChanged(int port, int pin, bool level) {
    if (static_cast<int>(lineSource[pin >> 2] >> (4 * (pin & 3)) & 0xF) != port)
        return;
    const std::uint32_t bit = 1u << pin;
    if (bit & (level ? rtsr_ : ftsr_))
        raise(bit);
}

std::uint32_t Exti::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return imr_;
    case 0x04: return emr_;
    case 0x08: return rtsr_;
    case 0x0C: return ftsr_;
    case 0x14: return pr_;
    default: return 0;
    }
}

void Exti::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset & ~3u) {
    case 0x00: imr_ = value; break;
    case 0x04: emr_ = value; break;
    case 0x08: rtsr_ = value; break;
    case 0x0C: ftsr_ = value; break;
    case 0x10: raise(value & imr_); break;  // SWIER
    case 0x14: pr_ &= ~value; break;
    default: break;
    }
}

std::uint32_t Afio::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return evcr_;
    case 0x04: return mapr_;
    case 0x08: case 0x0C: case 0x10: case 0x14: return exti_.lineSource[(offset - 8) >> 2];
    case 0x1C: return mapr2_;
    default: return 0;
    }
}

void Afio::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset & ~3u) {
    case 0x00: evcr_ = value; break;
    case 0x04: mapr_ = value; break;
    case 0x08: case 0x0C: case 0x10: case 0x14: exti_.lineSource[(offset - 8) >> 2] = value & 0xFFFF; break;
    case 0x1C: mapr2_ = value; break;
    default: break;
    }
}

// --- timers ---------------------------------------------------------------------

std::uint32_t Timer::counterAt(std::uint64_t now) const {
    if (!(cr1_ & 1))
        return stoppedCount_;
    return static_cast<std::uint32_t>((now - start_) / (psc_ + 1) % (arr_ + 1));
}

std::uint32_t Timer::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return cr1_;
    case 0x0C: return dier_;
    case 0x10: return sr_;
    case 0x24: return counterAt(core_.now());
    case 0x28: return psc_;
    case 0x2C: return arr_;
    default: return offset < 0x50 ? other_[offset >> 2] : 0;
    }
}

void Timer::write(std::uint32_t offset, std::uint32_t value, int) {
    reschedule();
    const std::uint64_t now = core_.now();
    switch (offset & ~3u) {
    case 0x00: {
        const bool wasRunning = cr1_ & 1;
        if (wasRunning && !(value & 1))
            stoppedCount_ = counterAt(now);
        cr1_ = value & 0x3FF;
        if (!wasRunning && (value & 1))
            start_ = now - std::uint64_t(stoppedCount_) * (psc_ + 1);
        break;
    }
    case 0x0C: dier_ = value; break;
    case 0x10: sr_ &= value; break;
    case 0x14:
        if (value & 1) {  // UG: restart the counter
            start_ = now;
            stoppedCount_ = 0;
            if (!(cr1_ & 4))
                sr_ |= 1;
        }
        break;
    case 0x24:
        stoppedCount_ = value & 0xFFFF;
        start_ = now - std::uint64_t(stoppedCount_) * (psc_ + 1);
        break;
    case 0x28: psc_ = value & 0xFFFF; break;
    case 0x2C: arr_ = value & 0xFFFF; break;
    default:
        if (offset < 0x50)
            other_[offset >> 2] = value;
        break;
    }
}

std::uint64_t Timer::nextEvent() const {
    if ((cr1_ & 1) && (dier_ & 1))
        return start_ + period();
    return UINT64_MAX;
}

void Timer::advance(std::uint64_t now) {
    if (!(cr1_ & 1) || now < start_ + period())
        return;
    start_ = now - (now - start_) % period();
    sr_ |= 1;
    if (dier_ & 1)
        core_.setIrqPending(irq_);
    if (cr1_ & 8) {  // one-pulse mode
        cr1_ &= ~1u;
        stoppedCount_ = 0;
    }
}

// --- DMA ------------------------------------------------------------------------

std::uint32_t Dma::read(std::uint32_t offset, int) {
    if (offset == 0) {
        // Three reads of the same value within a few cycles each: a wait loop.
        const std::uint64_t now = core_.now();
        polls_ = isr_ == polledIsr_ && now - polledAt_ < 64 ? polls_ + 1 : 0;
        polledIsr_ = isr_;
        polledAt_ = now;
        if (polls_ >= 2 && onIdlePoll)
            onIdlePoll();
        return isr_;
    }
    if (offset < 8 || offset >= 8 + 20 * channels_.size())
        return 0;
    const Channel& c = channels_[(offset - 8) / 20];
    switch ((offset - 8) % 20) {
    case 0x0: return c.ccr;
    case 0x4: {
        if (!(c.ccr & 1))
            return c.cndtr;
        if (c.completesAt == UINT64_MAX)
            return c.remaining;
        const std::uint64_t now = core_.now();
        const std::uint64_t per = cyclesFor((offset - 8) / 20);
        const std::uint64_t left = c.completesAt > now ? (c.completesAt - now + per - 1) / per : 0;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(left, c.remaining));
    }
    case 0x8: return c.cpar;
    case 0xC: return c.cmar;
    default: return 0;
    }
}

void Dma::write(std::uint32_t offset, std::uint32_t value, int) {
    reschedule();
    if (offset == 4) {  // IFCR: clearing GIFx clears all of the channel's flags
        for (std::size_t ch = 0; ch < channels_.size(); ++ch)
            if (value >> (4 * ch) & 1)
                value |= 0xFu << (4 * ch);
        isr_ &= ~value;
        return;
    }
    if (offset < 8 || offset >= 8 + 20 * channels_.size())
        return;
    Channel& c = channels_[(offset - 8) / 20];
    switch ((offset - 8) % 20) {
    case 0x0: {
        const bool wasEnabled = c.ccr & 1;
        c.ccr = value & 0x7FFF;
        if ((value & 1) && !wasEnabled) {
            c.remaining = c.cndtr;
            const bool paced = (value & 0x10) &&
                               std::find(pacedPeripherals.begin(), pacedPeripherals.end(), c.cpar) != pacedPeripherals.end();
            c.completesAt = c.cndtr && !paced ? core_.now() + cyclesFor(static_cast<std::size_t>(&c - channels_.data())) * c.cndtr : UINT64_MAX;
        } else if (!(value & 1)) {
            c.completesAt = UINT64_MAX;
        }
        break;
    }
    case 0x4: c.cndtr = value & 0xFFFF; break;
    case 0x8: c.cpar = value; break;
    case 0xC: c.cmar = value; break;
    default: break;
    }
}

std::optional<std::uint32_t> Dma::pull(std::uint32_t peripheral) {
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
        Channel& c = channels_[ch];
        if ((c.ccr & 0x11) != 0x11 || c.cpar != peripheral || c.remaining == 0)
            continue;
        const int msize = 1 << (c.ccr >> 10 & 3);
        const std::uint32_t index = c.cndtr - c.remaining;
        const std::uint32_t value = core_.read(c.cmar + ((c.ccr & 0x80) ? index * msize : 0), msize);
        if (--c.remaining == 0) {
            isr_ |= 0x7u << (4 * ch);
            if (c.ccr & 0x6)
                core_.setIrqPending(irqs_[ch]);
            if (c.ccr & 0x20)
                c.remaining = c.cndtr;
            else
                c.cndtr = 0;
        }
        return value;
    }
    return std::nullopt;
}

bool Dma::feeding(std::uint32_t peripheral) const {
    for (const Channel& c : channels_)
        if ((c.ccr & 0x11) == 0x11 && c.cpar == peripheral && c.remaining != 0)
            return true;
    return false;
}

std::uint64_t Dma::nextEvent() const {
    std::uint64_t next = UINT64_MAX;
    for (const Channel& c : channels_)
        next = std::min(next, c.completesAt);
    return next;
}

void Dma::advance(std::uint64_t now) {
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
        Channel& c = channels_[ch];
        if (c.completesAt > now)
            continue;
        if (static_cast<int>(ch) == holdWhileHandler && core_.handlerActive(16 + irqs_[ch])) {
            c.completesAt = now + kHoldRetry;  // look again shortly
            continue;
        }
        complete(static_cast<int>(ch));
    }
}

void Dma::complete(int ch) {
    Channel& c = channels_[ch];
    const int psize = 1 << (c.ccr >> 8 & 3), msize = 1 << (c.ccr >> 10 & 3);
    const bool toPeripheral = c.ccr & 0x10, pinc = c.ccr & 0x40, minc = c.ccr & 0x80;
    std::vector<std::uint32_t> items;
    items.reserve(c.remaining);
    for (std::uint32_t i = 0; i < c.remaining; ++i) {
        const std::uint32_t paddr = c.cpar + (pinc ? i * psize : 0), maddr = c.cmar + (minc ? i * msize : 0);
        if (toPeripheral) {
            const std::uint32_t v = core_.read(maddr, msize);
            items.push_back(v);
            core_.write(paddr, v, psize);
        } else {
            core_.write(maddr, core_.read(paddr, psize), msize);
        }
    }
    if (toPeripheral && onTransfer)
        onTransfer(c.cpar, items);

    if (ch == 0) {  // diagnostic
        if (core_.ipsr() == static_cast<std::uint32_t>(16 + irqs_[ch]))
            ++completionsInHandler;
        else if (isr_ & 0x2u)
            ++completionsUnacknowledged;
    }
    isr_ |= 0x7u << (4 * ch);  // GIF, TCIF, HTIF
    if (c.ccr & 0x6)
        core_.setIrqPending(irqs_[ch]);
    if (c.ccr & 0x20) {  // circular
        c.remaining = c.cndtr;
        c.completesAt = core_.now() + cyclesFor(static_cast<std::size_t>(ch)) * std::max<std::uint32_t>(c.cndtr, 1);
    } else {
        c.cndtr = 0;
        c.remaining = 0;
        c.completesAt = UINT64_MAX;
    }
}

// --- SPI ------------------------------------------------------------------------

void Spi::updateIrq() {
    if (((cr2_ & 0x40) && (sr_ & 1)) || ((cr2_ & 0x80) && (sr_ & 2)))
        core_.setIrqPending(irq_);
}

std::uint8_t Spi::exchange(std::uint8_t fromMaster) {
    std::uint8_t out = txLoaded_ ? tx_ : 0xFF;
    if (!txLoaded_ && (cr2_ & 2) && pullTx)
        if (const auto next = pullTx())
            out = static_cast<std::uint8_t>(*next);
    txLoaded_ = false;
    if (sr_ & 1)
    {
        sr_ |= 0x40;  // overrun
        ++overruns;
    }
    rx_ = fromMaster;
    sr_ |= 3;  // RXNE, TXE
    updateIrq();
    return out;
}

std::uint32_t Spi::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return cr1_;
    case 0x04: return cr2_;
    case 0x08: return sr_;
    case 0x0C:
        if ((sr_ & 1) && onDataRead)
            onDataRead();
        sr_ &= ~1u;
        return rx_;
    case 0x10: return 7;  // CRCPR reset value
    default: return 0;
    }
}

void Spi::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset & ~3u) {
    case 0x00: cr1_ = value; break;
    case 0x04:
        cr2_ = value;
        updateIrq();
        break;
    case 0x08: sr_ = (sr_ & ~0x10u) | (value & 0x10); break;
    case 0x0C:
        if (cr1_ & 4) {  // master
            rx_ = onMasterTransfer ? onMasterTransfer(static_cast<std::uint8_t>(value)) : 0xFF;
            sr_ |= 3;
        } else {
            tx_ = static_cast<std::uint8_t>(value);
            txLoaded_ = true;
            sr_ &= ~2u;
        }
        updateIrq();
        break;
    default: break;
    }
}

// --- USART ----------------------------------------------------------------------

void Usart::updateIrq() {
    if (((cr1_ & 0x20) && !rx_.empty()) || (cr1_ & 0x80) || ((cr1_ & 0x40) && (sr_ & 0x40)))
        core_.setIrqPending(irq_);
}

void Usart::receive(std::uint8_t byte) {
    rx_.push_back(byte);
    sr_ |= 0x20;
    updateIrq();
}

std::uint32_t Usart::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return sr_ | (rx_.empty() ? 0 : 0x20);
    case 0x04: {
        if (rx_.empty())
            return 0;
        const std::uint8_t byte = rx_.front();
        rx_.pop_front();
        if (rx_.empty())
            sr_ &= ~0x20u;
        return byte;
    }
    case 0x08: return brr_;
    case 0x0C: return cr1_;
    case 0x10: return cr2_;
    case 0x14: return cr3_;
    case 0x18: return gtpr_;
    default: return 0;
    }
}

void Usart::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset & ~3u) {
    case 0x00: sr_ &= value | ~0x360u; break;  // rc_w0 bits
    case 0x04:
        if (onTransmit)
            onTransmit(static_cast<std::uint8_t>(value));
        sr_ |= 0xC0;  // transmission is instantaneous
        updateIrq();
        break;
    case 0x08: brr_ = value; break;
    case 0x0C:
        cr1_ = value;
        updateIrq();
        break;
    case 0x10: cr2_ = value; break;
    case 0x14: cr3_ = value; break;
    case 0x18: gtpr_ = value; break;
    default: break;
    }
}

// --- ADC ------------------------------------------------------------------------

int Adc::sequenceChannel(int index) const {
    const std::uint32_t sqr1 = other_[(0x2C - 0x0C) >> 2], sqr2 = other_[(0x30 - 0x0C) >> 2], sqr3 = other_[(0x34 - 0x0C) >> 2];
    if (index < 6)
        return sqr3 >> (5 * index) & 0x1F;
    if (index < 12)
        return sqr2 >> (5 * (index - 6)) & 0x1F;
    return sqr1 >> (5 * (index - 12)) & 0x1F;
}

std::uint32_t Adc::read(std::uint32_t offset, int) {
    switch (offset & ~3u) {
    case 0x00: return sr_;
    case 0x04: return cr1_;
    case 0x08: return cr2_;
    case 0x4C: {
        const int length = (cr1_ & 0x100) ? static_cast<int>(other_[(0x2C - 0x0C) >> 2] >> 20 & 0xF) + 1 : 1;
        const int channel = sequenceChannel(index_ % length);
        index_ = (index_ + 1) % length;
        if (index_ == 0 && !(cr2_ & 2))
            sr_ &= ~2u;  // EOC
        return (sample ? sample(channel) : 2048) & 0xFFF;
    }
    default: return offset >= 0x0C && offset < 0x48 ? other_[(offset - 0x0C) >> 2] : 0;
    }
}

void Adc::write(std::uint32_t offset, std::uint32_t value, int) {
    switch (offset & ~3u) {
    case 0x00: sr_ &= value; break;
    case 0x04: cr1_ = value; break;
    case 0x08: {
        const bool wasOn = cr2_ & 1;
        cr2_ = value & 0xFF9FFFF3;  // calibration completes at once; start bits self-clear
        const bool start = (value & 0x400000) || (wasOn && (value & 1) && !(value & 0xC));
        if (start) {
            index_ = 0;
            sr_ |= 0x12;  // EOC, STRT
            if (cr1_ & 0x20)
                core_.setIrqPending(irq_);
        }
        break;
    }
    default:
        if (offset >= 0x0C && offset < 0x48)
            other_[(offset - 0x0C) >> 2] = value;
        break;
    }
}

}  // namespace hydra::stm32

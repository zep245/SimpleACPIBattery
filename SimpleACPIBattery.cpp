/*
 * SimpleACPIBattery - battery and charger for macOS on machines with a plain ACPI control-method battery (PNP0C0A) and AC
 * adapter (ACPI0003), without VirtualSMC's SMCBatteryManager. Written for a Xiaomi Tiger Lake laptop, whose
 * battery code reads the embedded controller through H_EC.ECRD: SSDT-ECAV must be loaded too, or every value reads 0.
 *
 * It attaches to the ACPI battery device, reads _STA/_BIF/_BST (and the adapter's _PSR) every 15 s and on every ACPI
 * notification, and publishes the result as an IOPMPowerSource - the object powerd turns into the menu-bar battery.
 * Everything it reads is logged ("SimpleACPIBattery:" in the kernel log) and kept in the ioreg property SimpleACPIBattery-ACPI.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSBoolean.h>

#define SAB_POLL_MS      15000
#define SAB_FAST_MS       3000
#define SAB_UNKNOWN 0xFFFFFFFFu

/* The power source itself. IOPMPowerSource needs its own init() (it builds its property table there), which an
   IOKit personality never calls - so the driver below matches the ACPI device and creates this object by hand. */
class SimpleACPIPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(SimpleACPIPowerSource)
public:
    static SimpleACPIPowerSource *make() {
        SimpleACPIPowerSource *p = new SimpleACPIPowerSource;
        if (p && !p->init()) { p->release(); p = nullptr; }
        return p;
    }
    void num(const char *key, unsigned long long v) {
        const OSSymbol *k = OSSymbol::withCString(key); OSNumber *n = OSNumber::withNumber(v, 32);
        if (k && n) setPSProperty(k, n);
        if (n) n->release(); if (k) k->release();
    }
    void flag(const char *key, bool v) {
        const OSSymbol *k = OSSymbol::withCString(key); if (k) { setPSProperty(k, const_cast<OSBoolean *>(v ? kOSBooleanTrue : kOSBooleanFalse)); k->release(); }
    }
    void text(int which, const char *v) {          /* 0 model, 1 serial, 2 manufacturer */
        const OSSymbol *s = OSSymbol::withCString(v); if (!s) return;
        OSSymbol *m = const_cast<OSSymbol *>(s);
        if (which == 0) setModel(m); else if (which == 1) setSerial(m); else setManufacturer(m);
        s->release();
    }
    /* IOPMPowerSource's setters are protected; the driver object calls them through these */
    using IOPMPowerSource::setExternalConnected; using IOPMPowerSource::setExternalChargeCapable;
    using IOPMPowerSource::setBatteryInstalled; using IOPMPowerSource::setIsCharging; using IOPMPowerSource::setAtWarnLevel;
    using IOPMPowerSource::setAtCriticalLevel; using IOPMPowerSource::setCurrentCapacity; using IOPMPowerSource::setMaxCapacity;
    using IOPMPowerSource::setTimeRemaining; using IOPMPowerSource::setAmperage; using IOPMPowerSource::setVoltage;
    using IOPMPowerSource::setCycleCount; using IOPMPowerSource::setLocation; using IOPMPowerSource::setPSProperty;
};
OSDefineMetaClassAndStructors(SimpleACPIPowerSource, IOPMPowerSource)

class SimpleACPIBattery : public IOService {
    OSDeclareDefaultStructors(SimpleACPIBattery)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn message(UInt32 type, IOService *provider, void *argument) override;
private:
    SimpleACPIPowerSource *ps_ = nullptr;
    IOACPIPlatformDevice *bat_ = nullptr;
    IOACPIPlatformDevice *ac_ = nullptr;
    IOWorkLoop *wl_ = nullptr;
    IOTimerEventSource *timer_ = nullptr;
    IONotifier *acNote_ = nullptr;
    UInt32 unitIsPower_ = 0, designCap_ = 0, fullCap_ = 0, designMV_ = 0, warnCap_ = 0, lowCap_ = 0;
    UInt32 polls_ = 0, lastState_ = 0xFFFFFFFFu;
    bool haveInfo_ = false, lastAC_ = false, logged_ = false;

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn acNotify(void *target, void *refCon, UInt32 type, IOService *provider, void *arg, vm_size_t size);
    void poll(const char *why);
    bool readInfo();
    static UInt32 num(OSArray *a, unsigned i);
    static void str(OSArray *a, unsigned i, char *out, size_t n);
    UInt32 toMAh(UInt32 v, UInt32 mV) const;
};

OSDefineMetaClassAndStructors(SimpleACPIBattery, IOService)

UInt32 SimpleACPIBattery::num(OSArray *a, unsigned i) {
    OSNumber *n = a ? OSDynamicCast(OSNumber, a->getObject(i)) : nullptr;
    return n ? n->unsigned32BitValue() : SAB_UNKNOWN;
}
void SimpleACPIBattery::str(OSArray *a, unsigned i, char *out, size_t n) {
    out[0] = 0;
    OSObject *o = a ? a->getObject(i) : nullptr;
    if (OSString *s = OSDynamicCast(OSString, o)) { strlcpy(out, s->getCStringNoCopy(), n); return; }
    if (OSData *d = OSDynamicCast(OSData, o)) {   /* a buffer where ACPI wants a string (this firmware's B1DN/B1SN/B1MN) */
        size_t len = d->getLength(); const char *p = (const char *)d->getBytesNoCopy();
        size_t k = 0; for (; p && k < len && k + 1 < n && p[k]; k++) out[k] = (p[k] >= 32 && p[k] < 127) ? p[k] : '?';
        out[k] = 0;
    }
}
UInt32 SimpleACPIBattery::toMAh(UInt32 v, UInt32 mV) const {
    if (v == SAB_UNKNOWN) return 0;
    if (!unitIsPower_) return v;                         /* already mAh */
    if (!mV || mV == SAB_UNKNOWN) mV = 11400;
    return (UInt32)(((UInt64)v * 1000) / mV);            /* mWh -> mAh */
}

bool SimpleACPIBattery::start(IOService *provider) {
    bat_ = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!bat_) { IOLog("SimpleACPIBattery: provider is not an ACPI device\n"); return false; }
    if (!IOService::start(provider)) return false;
    bat_->retain();
    ps_ = SimpleACPIPowerSource::make();
    if (!ps_ || !ps_->attach(this) || !ps_->start(this)) {
        IOLog("SimpleACPIBattery: could not create the power source\n");
        if (ps_) { ps_->release(); ps_ = nullptr; }
        bat_->release(); bat_ = nullptr; return false; }

    /* the AC adapter: the first ACPI0003 device */
    OSDictionary *m = IOService::nameMatching("ACPI0003");
    if (m) {
        IOService *s = IOService::waitForMatchingService(m, 2000ull * 1000ull * 1000ull);   /* does not consume m */
        m->release();
        ac_ = s ? OSDynamicCast(IOACPIPlatformDevice, s) : nullptr;
        if (s && !ac_) s->release();
        if (ac_) acNote_ = ac_->registerInterest(gIOGeneralInterest, &SimpleACPIBattery::acNotify, this, nullptr);
    }
    IOLog("SimpleACPIBattery: battery %s, AC adapter %s\n", bat_->getName(), ac_ ? ac_->getName() : "NOT FOUND");

    wl_ = getWorkLoop();
    if (wl_) { wl_->retain(); timer_ = IOTimerEventSource::timerEventSource(this, &SimpleACPIBattery::timerFired);
               if (timer_ && wl_->addEventSource(timer_) != kIOReturnSuccess) { timer_->release(); timer_ = nullptr; } }

    ps_->setLocation(0);
    ps_->setExternalChargeCapable(true);
    { const OSSymbol *k = OSSymbol::withCString("Type"); OSString *v = OSString::withCString("InternalBattery");
      if (k && v) ps_->setPSProperty(k, v); if (v) v->release(); if (k) k->release(); }
    ps_->setProperty("SimpleACPIBattery", "ACPI battery (PNP0C0A + ACPI0003)");
    ps_->registerService();
    registerService();
    poll("start");
    if (timer_) timer_->setTimeoutMS(SAB_FAST_MS);
    return true;
}

void SimpleACPIBattery::stop(IOService *provider) {
    if (timer_) { timer_->cancelTimeout(); if (wl_) wl_->removeEventSource(timer_); timer_->release(); timer_ = nullptr; }
    if (acNote_) { acNote_->remove(); acNote_ = nullptr; }
    if (wl_) { wl_->release(); wl_ = nullptr; }
    if (ac_) { ac_->release(); ac_ = nullptr; }
    if (ps_) { ps_->stop(this); ps_->detach(this); ps_->release(); ps_ = nullptr; }
    if (bat_) { bat_->release(); bat_ = nullptr; }
    IOService::stop(provider);
}

void SimpleACPIBattery::timerFired(OSObject *owner, IOTimerEventSource *sender) {
    SimpleACPIBattery *me = OSDynamicCast(SimpleACPIBattery, owner);
    if (!me) return;
    me->poll("timer");
    sender->setTimeoutMS(SAB_POLL_MS);
}

IOReturn SimpleACPIBattery::message(UInt32 type, IOService *provider, void *argument) {
    /* ACPI Notify(BAT0, 0x80/0x81) arrives as kIOACPIMessageDeviceNotification from the provider */
    if (timer_) timer_->setTimeoutMS(200);
    return IOService::message(type, provider, argument);
}
IOReturn SimpleACPIBattery::acNotify(void *target, void *, UInt32, IOService *, void *, vm_size_t) {
    SimpleACPIBattery *me = (SimpleACPIBattery *)target;
    if (me && me->timer_) me->timer_->setTimeoutMS(200);
    return kIOReturnSuccess;
}

bool SimpleACPIBattery::readInfo() {
    OSObject *o = nullptr;
    IOReturn r = bat_->evaluateObject("_BIF", &o);
    OSArray *a = OSDynamicCast(OSArray, o);
    if (r != kIOReturnSuccess || !a || a->getCount() < 13) {
        IOLog("SimpleACPIBattery: _BIF failed (0x%x, %u entries)\n", r, a ? a->getCount() : 0);
        if (o) o->release();
        return false;
    }
    unitIsPower_ = num(a, 0) == 0;
    designCap_ = num(a, 1); fullCap_ = num(a, 2); designMV_ = num(a, 4); warnCap_ = num(a, 5); lowCap_ = num(a, 6);
    char model[64], serial[64], type[32], oem[64];
    str(a, 9, model, sizeof model); str(a, 10, serial, sizeof serial); str(a, 11, type, sizeof type); str(a, 12, oem, sizeof oem);
    IOLog("SimpleACPIBattery: _BIF unit %s, design %u, full %u, design voltage %u mV, warn %u, low %u, model '%s' serial '%s' type '%s' maker '%s'\n",
          unitIsPower_ ? "mW/mWh" : "mA/mAh", designCap_, fullCap_, designMV_, warnCap_, lowCap_, model, serial, type, oem);
    if (model[0]) ps_->text(0, model);
    if (serial[0]) ps_->text(1, serial);
    if (oem[0]) ps_->text(2, oem);
    ps_->setProperty("SimpleACPIBattery-BIF", a);
    o->release();
    haveInfo_ = designCap_ != SAB_UNKNOWN && designCap_ != 0 && fullCap_ != SAB_UNKNOWN && fullCap_ != 0;
    return haveInfo_;
}

void SimpleACPIBattery::poll(const char *why) {
    if (!bat_ || !ps_) return;
    polls_++;
    UInt32 sta = 0;
    IOReturn rs = bat_->evaluateInteger("_STA", &sta);
    const bool present = rs == kIOReturnSuccess && (sta & 0x10);
    UInt32 psr = 0; bool ac = false;
    if (ac_ && ac_->evaluateInteger("_PSR", &psr) == kIOReturnSuccess) ac = psr != 0;

    if (!present) {
        if (!logged_ || polls_ % 20 == 1)
            IOLog("SimpleACPIBattery: %s: _STA 0x%x (0x%x) - battery NOT present%s\n", why, sta, rs,
                  sta == 0x0B ? " (0x0B: the EC reads 0 - is SSDT-ECAV loaded?)" : "");
        logged_ = true;
        ps_->setBatteryInstalled(false); ps_->setExternalConnected(true); ps_->setIsCharging(false);
        ps_->updateStatus(); ps_->messageClients(kIOPMMessageBatteryStatusHasChanged);
        return;
    }
    if (!haveInfo_ || polls_ % 40 == 0) readInfo();

    OSObject *o = nullptr;
    IOReturn r = bat_->evaluateObject("_BST", &o);
    OSArray *b = OSDynamicCast(OSArray, o);
    if (r != kIOReturnSuccess || !b || b->getCount() < 4) {
        IOLog("SimpleACPIBattery: %s: _BST failed (0x%x)\n", why, r);
        if (o) o->release();
        return;
    }
    const UInt32 state = num(b, 0), rate = num(b, 1), remain = num(b, 2), mV = num(b, 3);
    ps_->setProperty("SimpleACPIBattery-BST", b);
    o->release();

    const UInt32 voltage = (mV && mV != SAB_UNKNOWN) ? mV : designMV_;
    const UInt32 curMAh = toMAh(remain, voltage), fullMAh = toMAh(fullCap_, voltage), designMAh = toMAh(designCap_, designMV_);
    UInt32 rateMA = toMAh(rate, voltage);
    const bool charging = state & 2, discharging = state & 1, critical = state & 4;
    if (!ac_) ac = !discharging;                           /* no adapter device: infer it */
    const bool full = ac && !charging && fullMAh && curMAh * 100 >= fullMAh * 95;

    int amperage = (int)rateMA; if (discharging) amperage = -amperage; if (!charging && !discharging) amperage = 0;
    int minutes = 0;
    if (rateMA && rateMA != SAB_UNKNOWN) {
        if (discharging) minutes = (int)((UInt64)curMAh * 60 / rateMA);
        else if (charging && fullMAh > curMAh) minutes = (int)((UInt64)(fullMAh - curMAh) * 60 / rateMA);
    }
    const UInt32 pct = fullMAh ? (UInt32)((UInt64)curMAh * 100 / fullMAh) : 0;

    ps_->setBatteryInstalled(true);
    ps_->setExternalConnected(ac);
    ps_->setExternalChargeCapable(ac);
    ps_->setIsCharging(charging);
    ps_->flag("FullyCharged", full);
    ps_->setAtCriticalLevel(critical);
    ps_->setAtWarnLevel(warnCap_ != SAB_UNKNOWN && remain != SAB_UNKNOWN && remain <= warnCap_);
    ps_->setCurrentCapacity(curMAh);
    ps_->setMaxCapacity(fullMAh);
    ps_->setVoltage(voltage);
    ps_->setAmperage(amperage);
    ps_->setTimeRemaining(minutes);
    ps_->setCycleCount(0);
    /* the keys newer powerd reads besides the classic IOPMPowerSource ones */
    ps_->num("DesignCapacity", designMAh);
    ps_->num("AppleRawCurrentCapacity", curMAh);
    ps_->num("AppleRawMaxCapacity", fullMAh);
    ps_->num("NominalChargeCapacity", fullMAh);
    ps_->num("AppleRawBatteryVoltage", voltage);
    ps_->num("InstantAmperage", (unsigned long long)(long long)amperage);
    ps_->num("AvgTimeToEmpty", discharging ? minutes : 0xFFFF);
    ps_->num("AvgTimeToFull", charging ? minutes : 0xFFFF);
    ps_->num("InstantTimeToEmpty", discharging ? minutes : 0xFFFF);
    ps_->num("Temperature", 2980);                       /* 29.8 C: no sensor in ACPI */
    ps_->num("PermanentFailureStatus", 0);
    {   OSDictionary *ad = OSDictionary::withCapacity(2); const OSSymbol *k = OSSymbol::withCString("AdapterDetails");
        if (ad && k) { OSNumber *w = OSNumber::withNumber(ac ? 65 : 0, 32); if (w) { ad->setObject("Watts", w); w->release(); }
                       ps_->setPSProperty(k, ad); }
        if (ad) ad->release(); if (k) k->release(); }

    if (state != lastState_ || ac != lastAC_ || polls_ <= 2 || polls_ % 40 == 0)
        IOLog("SimpleACPIBattery: %s: _STA 0x%x _PSR %u _BST state 0x%x rate %u remaining %u voltage %u -> %u/%u mAh (%u%%), %d mA, %d min, %s\n",
              why, sta, psr, state, rate, remain, mV, curMAh, fullMAh, pct, amperage, minutes,
              charging ? "charging" : discharging ? "discharging" : ac ? "on AC, not charging" : "idle");
    lastState_ = state; lastAC_ = ac;

    ps_->updateStatus();
    ps_->messageClients(kIOPMMessageBatteryStatusHasChanged);
}

/* ---- kext entry: the kmod_info record macOS requires ("kexts must have a _kmod_info symbol"). libkmod's _start/_stop
   run the C++ constructors and then _realmain/_antimain (none needed here). Xcode generates this; a hand build must
   carry it. */
#include <mach/kmod.h>
extern "C" {
kern_return_t _start(kmod_info_t *ki, void *data);
kern_return_t _stop(kmod_info_t *ki, void *data);
KMOD_EXPLICIT_DECL(local.SimpleACPIBattery, "1.0.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
}

// P1.7: prove several MidiTransports can feed the SAME InstrumentController, and that
// each transport tags its events with a distinct source. Uses a host fake transport
// (no Arduino) exercising the transport-neutral interface.
#include "TestFramework.h"
#include "../src/core/configuration/Profile.h"
#include "../src/core/instrument/InstrumentController.h"
#include "../src/core/midi/MidiTransport.h"

using namespace gmb;

// A minimal host transport: emits a scripted batch of events on the next poll().
class FakeTransport : public MidiTransport {
public:
    FakeTransport(MidiSource src, const char* name) : src_(src), name_(name) {}
    void queue(const MidiEvent& e) { pending_.push_back(e); }
    void poll(uint32_t) override {
        events_ = pending_;
        for (auto& e : events_) e.source = static_cast<uint8_t>(src_);  // transport stamps it
        pending_.clear();
    }
    std::vector<MidiEvent>& events() override { return events_; }
    void clear() override { events_.clear(); }
    MidiSource source() const override { return src_; }
    const char* name() const override { return name_; }
private:
    MidiSource src_;
    const char* name_;
    std::vector<MidiEvent> pending_, events_;
};

static Profile uke() {
    return Profile::makeDefault("Ukulele", 4, {67, 60, 64, 69}, 12);
}
static MidiEvent noteOn(uint8_t note, uint8_t vel) {
    MidiEvent e; e.type = (uint8_t)MidiType::NoteOn;
    e.channel = 0; e.data1 = note; e.data2 = vel; return e;
}

TEST(multiple_transports_feed_one_controller) {
    InstrumentController ic;
    ic.load(uke());  // open notes 67,60,64,69

    FakeTransport udp(MidiSource::WifiUdp, "wifiUdp");
    FakeTransport usb(MidiSource::Usb, "usb");
    MidiTransport* transports[] = {&udp, &usb};

    // Two different open strings arrive from two different transports.
    udp.queue(noteOn(60, 100));  // C (string 1)
    usb.queue(noteOn(69, 100));  // A (string 3)

    // The exact loop main.cpp runs: poll every transport, route every event into the
    // one controller.
    std::vector<uint8_t> seenSources;
    for (MidiTransport* t : transports) {
        t->poll(0);
        for (auto& e : t->events()) {
            seenSources.push_back(e.source);
            ic.handleEvent(e, 0);
        }
        t->clear();
    }
    ic.tick(5000);  // flush the chord window

    // Both notes, arriving over two transports, played on the one instrument.
    CHECK_EQ(ic.soundingCount(), 2);
    // And each event carried its transport's source tag (diagnostics/routing).
    CHECK_EQ((int)seenSources.size(), 2);
    CHECK_EQ(seenSources[0], (uint8_t)MidiSource::WifiUdp);
    CHECK_EQ(seenSources[1], (uint8_t)MidiSource::Usb);

    // Interface identity is exposed for diagnostics.
    CHECK(udp.source() == MidiSource::WifiUdp);
    CHECK(usb.source() == MidiSource::Usb);
    CHECK(std::string(usb.name()) == "usb");
    // clear() emptied each batch.
    CHECK_EQ((int)udp.events().size(), 0);
}

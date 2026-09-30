/*
 * A module built from the test tree rather than from packages/modules, so the
 * suite covers the one thing it cannot otherwise reach: what a plugin can
 * actually link. It depends on the ABI target alone — adding a dependency on
 * ResonateEngine.Module would hide exactly the mistake it exists to catch — and
 * it uses a signal and a message stream, whose entry points live in the host.
 *
 * Its second job is to leave storage behind, because "the host reclaims what a
 * module did not destroy" is only observable from the host's log.
 */

#include <functional>

#include <resonate/module/module.hpp>

namespace
{

struct Loaded
{
    static constexpr std::uint32_t typeId()
    {
        return 1U;
    }

    std::uint32_t value;
};

/* Module state, held the way the shipped modules hold it: taken from the host, so
   the host is what reclaims it. */
struct ProbeState
{
    std::uint32_t value = 0;
};

/* Owns one block of it, and releases the block from its destructor: that is what
   makes the host report a release of memory it no longer holds once it has
   reclaimed the same block at detach. */
class StateBlock
{
  public:
    ~StateBlock()
    {
        release();
    }

    ResonateStatus take(const resonate::Host& host)
    {
        api_ = host.raw();
        memory_ = host.allocate<ProbeState>();
        return memory_ != nullptr ? RESONATE_OK : RESONATE_E_INTERNAL;
    }

  private:
    void release()
    {
        if (api_ != nullptr && memory_ != nullptr)
        {
            api_->deallocate(api_->user_data, memory_, sizeof(ProbeState));
            memory_ = nullptr;
        }
    }

    const ResonateHostApi* api_ = nullptr;
    ProbeState* memory_ = nullptr;
};

class ProbeModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        if (signalRoundTrip(host) != RESONATE_OK || messageRoundTrip(host) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        /* Left behind on purpose, all of it. The host has to reclaim the state,
           the signal and the writer, and the reader stays open on the writer so
           that its destruction is what closes it. */
        if (leaked_state_.take(host) != RESONATE_OK || leaked_signal_.create(host) != RESONATE_OK ||
            leaked_writer_.create(host, 4U) != RESONATE_OK ||
            leaked_writer_.open(leaked_stream_) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    }

    void onDetach() override
    {
        /* The two that are not reclaimed: released here, the way a module that
           cleans up after itself does it. */
        signal_.destroy();
        writer_.destroy();
    }

  private:
    resonate::Status signalRoundTrip(const resonate::Host& host)
    {
        if (signal_.create(host) != RESONATE_OK || node_.connect(signal_, handler_) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        signal_.emit(3);
        if (calls_ != 3 || signal_.subscriberCount() != 1U)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    }

    resonate::Status messageRoundTrip(const resonate::Host& host)
    {
        if (writer_.create(host, 4U) != RESONATE_OK || writer_.open(stream_) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        /* Closed explicitly, so the path that leaves one open is the leaked
           writer's. */
        resonate::MessageReader<Loaded> reader;
        reader.open(host, stream_);

        const Loaded sent = {42U};
        Loaded received = {};
        if (writer_.write(sent) == 0U || !reader.read(received) || received.value != 42U)
        {
            return RESONATE_E_INTERNAL;
        }

        reader.close();
        return RESONATE_OK;
    }

    int calls_ = 0;
    std::function<void(int)> handler_ = [this](int value) { calls_ += value; };

    resonate::Signal<int> signal_;
    resonate::SignalNode node_;

    resonate::MessageWriter<Loaded> writer_;
    ResonateMessageStream stream_ = {};

    resonate::Signal<int> leaked_signal_;
    resonate::MessageWriter<Loaded> leaked_writer_;
    ResonateMessageStream leaked_stream_ = {};
    StateBlock leaked_state_;
};

} // namespace

RESONATE_DEFINE_MODULE(ProbeModule, "resonate.probe", "Probe", "0.1.0")

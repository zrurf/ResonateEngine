#include "capability.h"

#include <resonate/module/module.hpp>

namespace
{

using resonate::physics::State;

class PhysicsModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        host_ = host.raw();

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        ResonatePhysicsWorld& published = resonate::physics::world();
        published.self = state_;
        return host.publish(published);
    }

    void onDetach() override
    {
        ResonatePhysicsWorld& published = resonate::physics::world();
        resonate::Host host(host_);
        host.unpublish(published);
        published.self = nullptr;
        host.deallocate(state_);
        state_ = nullptr;
    }

  private:
    State* state_ = nullptr;

    /* Kept because onDetach receives no host. */
    const ResonateHostApi* host_ = nullptr;
};

} // namespace

RESONATE_DEFINE_MODULE(PhysicsModule, "resonate.physics", "Physics", "0.1.0")

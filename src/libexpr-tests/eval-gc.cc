#include "nix/expr/eval-gc.hh"

#include <gtest/gtest.h>

#if NIX_USE_BOEHMGC
#  include <cstdint>
#  include <thread>

namespace nix {

TEST(BoehmGC, correctsMainAndWorkerStacks)
{
    initGC();

    auto checkStack = [] {
        int onStack;
        struct Probe
        {
            GC_sp_corrector_proc corrector;
            void * thread;
            void * native;
            void * outside = nullptr;
        } probe{GC_get_sp_corrector(), reinterpret_cast<void *>(pthread_self()), &onStack};

        // Match Boehm's calling convention: the callback runs with its lock
        // held, and may need to replace a pointer on a coroutine's stack.
        GC_call_with_alloc_lock(
            [](void * data) -> void * {
                auto & probe = *static_cast<Probe *>(data);
                probe.corrector(&probe.native, probe.thread);
                probe.corrector(&probe.outside, probe.thread);
                return nullptr;
            },
            &probe);
        EXPECT_EQ(probe.native, &onStack);
        EXPECT_NE(probe.outside, nullptr);
        EXPECT_LT(reinterpret_cast<uintptr_t>(probe.outside), reinterpret_cast<uintptr_t>(&onStack));
    };

    checkStack();
    // Repeated registration and collection also exercise removal of records
    // whose storage belongs to an exited worker's stack.
    for (int i = 0; i < 10; ++i) {
        std::thread thread([&] {
            BoehmThreadStack stack;
            GC_stack_base base;
            ASSERT_EQ(GC_get_stack_base(&base), GC_SUCCESS);
            ASSERT_EQ(GC_register_my_thread(&base), GC_SUCCESS);
            checkStack();
            GC_gcollect();
            GC_unregister_my_thread();
        });
        thread.join();
        GC_gcollect();
        checkStack();
    }
}

} // namespace nix
#endif

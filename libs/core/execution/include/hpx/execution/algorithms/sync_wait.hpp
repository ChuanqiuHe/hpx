//  Copyright (c) 2020 ETH Zurich
//  Copyright (c) 2022 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <hpx/config.hpp>
#include <hpx/concepts/concepts.hpp>
#include <hpx/datastructures/optional.hpp>
#include <hpx/datastructures/tuple.hpp>
#include <hpx/datastructures/variant.hpp>
#include <hpx/execution/algorithms/detail/partial_algorithm.hpp>
#include <hpx/execution/algorithms/detail/single_result.hpp>
#include <hpx/execution_base/completion_signatures.hpp>
#include <hpx/execution_base/operation_state.hpp>
#include <hpx/execution_base/sender.hpp>
#include <hpx/functional/detail/tag_priority_invoke.hpp>
#include <hpx/synchronization/condition_variable.hpp>
#include <hpx/synchronization/spinlock.hpp>
#include <hpx/type_support/meta.hpp>
#include <hpx/type_support/pack.hpp>
#include <hpx/type_support/unused.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <type_traits>
#include <utility>

enum class Sync_wait_type
    {
        single,
        variant
    };

namespace hpx::execution::experimental::detail {

    struct sync_wait_error_visitor
    {
        void operator()(std::exception_ptr ep) const
        {
            std::rethrow_exception(ep);
        }

        template <typename Error>
        void operator()(Error& error) const
        {
            throw error;
        }
    };

    template <typename Pack>
    struct make_decayed_pack;

    template <template <typename...> typename Pack, typename... Ts>
    struct make_decayed_pack<Pack<Ts...>>
    {
        using type = Pack<std::decay_t<Ts>...>;
    };

    template <typename Pack>
    using make_decayed_pack_t = typename make_decayed_pack<Pack>::type;

    // integrate sync_wait and sync_wait_with_variant
    //enum class Sync_wait_type
    //{
    //    single,
    //    variant
    //};
    // Sync_wait_type Type;
    template <typename Sender, Sync_wait_type Type>
    struct sync_wait_receiver
    {
        // value and error_types of the predecessor sender
        template <template <typename...> class Tuple,
            template <typename...> class Variant>
        using predecessor_value_types =
            value_types_of_t<Sender, empty_env, Tuple, Variant>;

        template <template <typename...> class Variant>
        using predecessor_error_types =
            error_types_of_t<Sender, empty_env, Variant>;

        // forcing static_assert ensuring variant has exactly one tuple
        //
        // FIXME: using make_decayed_pack is a workaround for the impedance
        // mismatch between the different techniques we use for calculating
        // value_types for a sender. In particular, split() explicitly adds a
        // const& to all tuple members in a way that prevent simply passing
        // decayed_tuple to predecessor_value_types.
        using single_result_type = make_decayed_pack_t<
            single_variant_t<predecessor_value_types<hpx::tuple, meta::pack>>>;

        // The template should compute the result type of whatever returned from
        // sync_wait or sync_wait_with_variant by checking Sync_wait_type is single or variant
        using result_type =
            std::conditional<Type == Sync_wait_type::single,
                hpx::variant<single_result_type>,
                predecessor_value_types<hpx::tuple, hpx::variant>>;

        // The type of errors to store in the variant. This in itself is a
        // variant.
        using error_type =
            hpx::util::detail::unique_t<hpx::util::detail::prepend_t<
                predecessor_error_types<hpx::variant>, std::exception_ptr>>;

        // We use a spinlock here to allow taking the lock on non-HPX threads.
        using mutex_type = hpx::spinlock;

        struct shared_state
        {
            // We use a spinlock here to allow taking the lock on non-HPX
            // threads.
            hpx::condition_variable cond_var;
            mutex_type mtx;
            std::atomic<bool> set_called = false;
            hpx::variant<hpx::monostate, error_type, result_type> value;

            void wait()
            {
                if (!set_called)
                {
                    std::unique_lock<mutex_type> l(mtx);
                    if (!set_called)
                    {
                        cond_var.wait(l);
                    }
                }
            }

            auto get_value()
            {
                if constexpr (Type == Sync_wait_type::single)
                {
                    return hpx::optional<single_result_type>(
                        hpx::get<0>(hpx::get<result_type>(HPX_MOVE(value))));
                }
                else if constexpr (Type == Sync_wait_type::variant)
                {
                    return hpx::optional(
                        hpx::get<result_type>(HPX_MOVE(value)));
                }

                if (hpx::holds_alternative<error_type>(value))
                {
                    hpx::visit(
                        sync_wait_error_visitor{}, hpx::get<error_type>(value));
                }

                // If the variant holds a hpx::monostate set_stopped was called
                // we return an empty optional
                return hpx::optional<result_type>();
            }
        };

        shared_state& state;

        void signal_set_called() noexcept
        {
            std::unique_lock<mutex_type> l(state.mtx);
            state.set_called = true;
            hpx::util::ignore_while_checking<decltype(l)> il(&l);
            HPX_UNUSED(il);

            state.cond_var.notify_one();
        }

        template <typename Error>
        friend void tag_invoke(
            set_error_t, sync_wait_receiver&& r, Error&& error) noexcept
        {
            r.state.value.template emplace<error_type>(
                HPX_FORWARD(Error, error));
            r.signal_set_called();
        }

        friend void tag_invoke(set_stopped_t, sync_wait_receiver&& r) noexcept
        {
            r.signal_set_called();
        }

        // possible add it back
        template <typename... Us>
        friend void tag_invoke(
            set_value_t, sync_wait_receiver&& r, Us&&... us) noexcept
        {
            r.state.value.template emplace<result_type>(
                hpx::forward_as_tuple(HPX_FORWARD(Us, us)...));
            r.signal_set_called();
        }
    };
}    // namespace hpx::execution::experimental::detail

namespace hpx::this_thread::experimental {
    
    // this_thread::sync_wait is a sender consumer that submits the work
    // described by the provided sender for execution, similarly to
    // ensure_started, except that it blocks the current std::thread or thread
    // of main until the work is completed, and returns an optional tuple of
    // values that were sent by the provided sender on its completion of work.
    // Where 4.20.1 execution::schedule and 4.20.3 execution::transfer_just are
    // meant to enter the domain of senders, sync_wait is meant to exit the
    // domain of senders, retrieving the result of the task graph.
    //
    // If the provided sender sends an error instead of values, sync_wait throws
    // that error as an exception, or rethrows the original exception if the
    // error is of type std::exception_ptr.
    //
    // If the provided sender sends the "stopped" signal instead of values,
    // sync_wait returns an empty optional.
    //
    // For an explanation of the requires clause, see 5.8 All senders are typed.
    // That clause also explains another sender consumer, built on top of
    // sync_wait: sync_wait_with_variant.
    //
    // Note: This function is specified inside hpx::this_thread::experimental,
    // and not inside hpx::execution::experimental. This is because sync_wait
    // has to block the current execution agent, but determining what the
    // current execution agent is is not reliable. Since the standard does not
    // specify any functions on the current execution agent other than those in
    // std::this_thread, this is the flavor of this function that is being
    // proposed.

    // this_thread::sync_wait and this_thread::sync_wait_with_variant are used
    // to block a current thread until a sender passed into it as an argument
    // has completed, and to obtain the values (if any) it completed with.
    //
    // For any receiver r created by an implementation of sync_wait and
    // sync_wait_with_variant, the expressions get_scheduler(get_env(r)) and
    // get_delegatee_scheduler(get_env(r)) shall be well-formed. For a receiver
    // created by the default implementation of this_thread::sync_wait, these
    // expressions shall return a scheduler to the same thread-safe,
    // first-in-first-out queue of work such that tasks scheduled to the queue
    // execute on the thread of the caller of sync_wait. [Note: The scheduler
    // for an instance of execution::run_loop that is a local variable within
    // sync_wait is one valid implementation. -- end note]
    //
    // The templates sync-wait-type and sync-wait-with-variant-type are used to
    // determine the return types of this_thread::sync_wait and
    // this_thread::sync_wait_with_variant. Let sync-wait-env be the type of the
    // expression get_env(r) where r is an instance of the receiver created by
    // the default implementation of sync_wait. Then:
    //
    // template<sender<sync-wait-env> S> using sync-wait-type =
    //  optional<execution::value_types_of_t< S, sync-wait-env, decayed-tuple,
    //  type_identity_t>>;
    //
    //  template<sender<sync-wait-env> S> using sync-wait-with-variant-type =
    //  optional<execution::into-variant-type<S, sync-wait-env>>;
    //
    // The name this_thread::sync_wait denotes a customization point object. For
    // some subexpression s, let S be decltype((s)). If execution::sender<S,
    // sync-wait-env> is false, or the number of the arguments
    // completion_signatures_of_t<S, sync-wait-env>::value_types passed into the
    // Variant template parameter is not 1, this_thread::sync_wait is
    // ill-formed. Otherwise, this_thread::sync_wait is expression-equivalent
    // to:
    //
    // 1. tag_invoke(this_thread::sync_wait,
    //          execution::get_completion_scheduler< execution::set_value_t>(s),
    //          s), if this expression is valid.
    //
    //      - Mandates: The type of the tag_invoke expression above is
    //                  sync-wait-type<S, sync-wait-env>.
    //
    // 2. Otherwise, tag_invoke(this_thread::sync_wait, s), if this expression
    //    is valid and its type is.
    //
    //      - Mandates: The type of the tag_invoke expression above is
    //                  sync-wait-type<S, sync-wait-env>.
    //
    // 3. Otherwise:
    //
    //      1. Constructs a receiver r.
    //
    //      2. Calls execution::connect(s, r), resulting in an operation state
    //         op_state, then calls execution::start(op_state).
    //
    //      3. Blocks the current thread until a receiver completion-signal of r
    //         is called. When it is:
    //
    //          1. If execution::set_value(r, ts...) has been called, returns
    //              sync-wait-type<S, sync-wait-env>{
    //                  decayed-tuple<decltype(ts)...>{ts...}}.
    //              If that expression exits exceptionally, the exception is
    //              propagated to the caller of sync_wait.
    //
    //          2. If execution::set_error(r, e) has been called, let E be the
    //             decayed type of e. If E is exception_ptr, calls
    //             std::rethrow_exception(e). Otherwise, if the E is error_code,
    //             throws system_error(e). Otherwise, throws e.
    //
    //          3. If execution::set_stopped(r) has been called, returns
    //             sync-wait-type<S, sync-wait-env>{}.
    //

    inline constexpr struct sync_wait_t final
      : hpx::functional::detail::tag_priority<sync_wait_t>
    {
    private:
        // clang-format off
        template <typename Sender, Sync_wait_type Type,
            HPX_CONCEPT_REQUIRES_(
                hpx::execution::experimental::is_sender_v<Sender> &&
                hpx::execution::experimental::detail::
                    is_completion_scheduler_tag_invocable_v<
                        hpx::execution::experimental::set_value_t,
                        Sender, Type, sync_wait_t
                    >
            )>
        // clang-format on
        friend constexpr HPX_FORCEINLINE auto tag_override_invoke(
            sync_wait_t, Sender&& sender)
        {
            auto scheduler =
                hpx::execution::experimental::get_completion_scheduler<
                    hpx::execution::experimental::set_value_t>(sender);

            return hpx::functional::tag_invoke(sync_wait_t{},
                HPX_MOVE(scheduler), HPX_FORWARD(Sender, sender));
        }

        // clang-format off
        template <typename Sender, Sync_wait_type Type,
            HPX_CONCEPT_REQUIRES_(
                hpx::execution::experimental::is_sender_v<Sender>
            )>
        // clang-format on
        friend constexpr HPX_FORCEINLINE auto tag_fallback_invoke(
            sync_wait_t, Sender&& sender)
        {
            using receiver_type =
                hpx::execution::experimental::detail::sync_wait_receiver<
                    Sender, Type>;
            using state_type = typename receiver_type::shared_state;

            state_type state{};
            auto op_state = hpx::execution::experimental::connect(
                HPX_FORWARD(Sender, sender), receiver_type{state});
            hpx::execution::experimental::start(op_state);

            state.wait();
            return state.get_value();
        }

        friend constexpr HPX_FORCEINLINE auto tag_fallback_invoke(sync_wait_t)
        {
            return hpx::execution::experimental::detail::partial_algorithm<
                sync_wait_t>{};
        }
    } sync_wait{};
}    // namespace hpx::this_thread::experimental

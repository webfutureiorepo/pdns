/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "mtasker_context.hh"
#include <exception>
#include <cassert>
#include <type_traits>
#include <boost/version.hpp>
#if BOOST_VERSION < 106100
#include <boost/context/fcontext.hpp>
using boost::context::make_fcontext;
#else
#include <boost/context/detail/fcontext.hpp>
using boost::context::detail::make_fcontext;
#endif /* BOOST_VERSION < 106100 */

// __CET__ is set by the compiler if relevant, so far only relevant/tested for amd64 on OpenBSD
#if defined(__amd64__)
#if __CET__ & 0x1
#define CET_ENDBR __asm("endbr64")
#else
#define CET_ENDBR
#endif
#else
#define CET_ENDBR
#endif

#ifdef PDNS_USE_VALGRIND
#include <valgrind/valgrind.h>
#endif /* PDNS_USE_VALGRIND */

#ifdef HAVE_FIBER_SANITIZER
__thread void* t_mainStack{nullptr};
__thread size_t t_mainStackSize{0};
#endif /* HAVE_FIBER_SANITIZER */

#if BOOST_VERSION < 105600
/* Note: This typedef means functions taking fcontext_t*, like jump_fcontext(),
 * now require a reinterpret_cast rather than a static_cast, since we're
 * casting from pdns_context_t->uc_mcontext, which is void**, to
 * some_opaque_struct**. In later versions fcontext_t is already void*. So if
 * you remove this, then fix the ugly.
 */
using fcontext_t = boost::context::fcontext_t*;

/* Emulate the >= 1.56 API for Boost 1.52 through 1.55 */
static inline intptr_t
jump_fcontext(fcontext_t* const ofc, fcontext_t const nfc,
              intptr_t const arg)
{
  /* If the fcontext_t is preallocated then use it, otherwise allocate one
   * on the stack ('self') and stash a pointer away in *ofc so the returning
   * MThread can access it. This is safe because we're suspended, so the
   * context object always outlives the jump.
   */
  if (*ofc) {
    return boost::context::jump_fcontext(*ofc, nfc, arg);
  }
  else {
    boost::context::fcontext_t self;
    *ofc = &self;
    auto ret = boost::context::jump_fcontext(*ofc, nfc, arg);
    *ofc = nullptr;
    return ret;
  }
}
#else

#if BOOST_VERSION < 106100
using boost::context::fcontext_t;
using boost::context::jump_fcontext;
#else
using boost::context::detail::fcontext_t;
using boost::context::detail::jump_fcontext;
using boost::context::detail::transfer_t;
#endif /* BOOST_VERSION < 106100 */

static_assert(std::is_pointer_v<fcontext_t>,
              "Boost Context has changed the fcontext_t type again :-(");
#endif

/* Boost context only provides a means of passing a single argument across a
 * jump. args_t simply provides a way to pass more by reference.
 */
struct args_t
{
#if BOOST_VERSION < 106100
  fcontext_t prev_ctx = nullptr;
#endif
  pdns_ucontext_t* self = nullptr;
  std::function<void(void)>* work = nullptr;
};

extern "C"
{
  static void
#if BOOST_VERSION < 106100
  threadWrapper(intptr_t const xargs)
  {
#else
  // If you see asan trouble in this function, run with
  // ASAN_OPTIONS=detect_stack_use_after_return=0 or completely disable it by compiling with
  // -fsanitize-address-use-after-return=never.  On debian clang versions up and including 14 do
  // not seem to trigger a problem here, but starting from version 15 they do.
  // Attempts at using function attributes to silence the error did not work.
  threadWrapper(transfer_t const theThread)
  {
#endif
    /* Access the args passed from pdns_makecontext, and copy them directly from
     * the calling stack on to ours (we're now using the MThreads stack).
     * This saves heap allocating an args object, at the cost of an extra
     * context switch to fashion this constructor-like init phase. The work
     * function object is still only moved after we're (re)started, so may
     * still be set or changed after a call to pdns_makecontext. This matches
     * the behaviour of the System V implementation, which can inherently only
     * be passed ints and pointers.
     */
    notifyStackSwitchDone();
#if BOOST_VERSION < 106100
    auto* args = reinterpret_cast<args_t*>(xargs);
#else
    auto* args = static_cast<args_t*>(theThread.data);
#endif
    auto* ctx = args->self;
    auto* work = args->work;
    /* we switch back to pdns_makecontext() */
    notifyStackSwitchToKernel();
#if BOOST_VERSION < 106100
    jump_fcontext(reinterpret_cast<fcontext_t*>(&ctx->uc_mcontext),
                  static_cast<fcontext_t>(args->prev_ctx), 0);
#else
    transfer_t res = jump_fcontext(theThread.fctx, nullptr);
    CET_ENDBR;
    /* we got switched back from pdns_swapcontext() */
    if (res.data != nullptr) {
      /* if res.data is not a nullptr, it holds a pointer to the context
         we just switched from, and we need to fill it to be able to
         switch back to it later. */
      auto* ptr = static_cast<fcontext_t*>(res.data);
      *ptr = res.fctx;
    }
#endif
    auto start = std::move(*work);
    notifyStackSwitchDone();
    args = nullptr;

    try {
      auto localstart = std::move(start);
      localstart();
    }
    catch (...) {
      ctx->exception = std::current_exception();
    }

    notifyStackSwitchToKernel();
    /* Emulate the System V uc_link feature. */
    auto* const next_ctx = ctx->uc_link->uc_mcontext;
#if BOOST_VERSION < 106100
    jump_fcontext(reinterpret_cast<fcontext_t*>(&ctx->uc_mcontext),
                  static_cast<fcontext_t>(next_ctx),
                  reinterpret_cast<intptr_t>(ctx));
#else
    jump_fcontext(static_cast<fcontext_t>(next_ctx), nullptr);
#endif

#ifdef NDEBUG
    __builtin_unreachable();
#endif
  }
}

pdns_ucontext_t::pdns_ucontext_t() :
  uc_mcontext(nullptr), uc_link(nullptr)
{
#ifdef PDNS_USE_VALGRIND
  valgrind_id = 0;
#endif /* PDNS_USE_VALGRIND */
}

#ifdef PDNS_USE_VALGRIND
pdns_ucontext_t::~pdns_ucontext_t()
{
  /* There's nothing to delete here since fcontext doesn't require anything
   * to be heap allocated.
   */
  if (valgrind_id != 0) {
    VALGRIND_STACK_DEREGISTER(valgrind_id);
  }
}
#else
pdns_ucontext_t::~pdns_ucontext_t() = default;
#endif /* PDNS_USE_VALGRIND */

void pdns_swapcontext(pdns_ucontext_t& __restrict octx, pdns_ucontext_t const& __restrict ctx)
{
  /* we either switch back to threadwrapper() if it's the first time,
     or we switch back to pdns_swapcontext(),
     in both case we will be returning from a call to jump_fcontext(). */
#if BOOST_VERSION < 106100
  intptr_t ptr = jump_fcontext(reinterpret_cast<fcontext_t*>(&octx.uc_mcontext),
                               static_cast<fcontext_t>(ctx.uc_mcontext), 0);

  auto origctx = reinterpret_cast<pdns_ucontext_t*>(ptr);
  if (origctx && origctx->exception)
    std::rethrow_exception(origctx->exception);
#else
  transfer_t res = jump_fcontext(static_cast<fcontext_t>(ctx.uc_mcontext), static_cast<void*>(&octx.uc_mcontext));
  CET_ENDBR;
  if (res.data != nullptr) {
    /* if res.data is not a nullptr, it holds a pointer to the context
       we just switched from, and we need to fill it to be able to
       switch back to it later. */
    auto* ptr = static_cast<fcontext_t*>(res.data);
    *ptr = res.fctx;
  }
  if (ctx.exception) {
    std::rethrow_exception(ctx.exception);
  }
#endif
}

void pdns_makecontext(pdns_ucontext_t& ctx, std::function<void(void)>& start)
{
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  assert(ctx.uc_link);
  assert(ctx.uc_stack.size() >= 8192);
  assert(!ctx.uc_mcontext);
  // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  ctx.uc_mcontext = make_fcontext(&ctx.uc_stack[ctx.uc_stack.size() - 1],
                                  ctx.uc_stack.size() - 1, &threadWrapper);
  args_t args;
  args.self = &ctx;
  args.work = &start;
  /* jumping to threadwrapper */
  notifyStackSwitch(&ctx.uc_stack[ctx.uc_stack.size() - 1], ctx.uc_stack.size() - 1);
#if BOOST_VERSION < 106100
  jump_fcontext(reinterpret_cast<fcontext_t*>(&args.prev_ctx),
                static_cast<fcontext_t>(ctx.uc_mcontext),
                reinterpret_cast<intptr_t>(&args));
#else
  transfer_t res = jump_fcontext(static_cast<fcontext_t>(ctx.uc_mcontext),
                                 &args);
  CET_ENDBR;
  /* back from threadwrapper, updating the context */
  ctx.uc_mcontext = res.fctx;
#endif
  notifyStackSwitchDone();
}

#pragma once

#include <optional>

/// Checked access to an optional a GoogleTest assertion has already proven
/// engaged.
///
/// The problem this solves. The ordinary test idiom is
///
/// ```
/// ASSERT_TRUE(decoded.has_value());
/// EXPECT_EQ(decoded->field, expected);
/// ```
///
/// which is correct — `ASSERT_TRUE` returns from the test on failure, so the
/// dereference below it cannot run on a disengaged optional. But
/// `bugprone-unchecked-optional-access` cannot see that: the early return is
/// buried inside GoogleTest's macro expansion, so its dataflow analysis
/// treats every such access as unchecked and the repository's
/// `WarningsAsErrors: '*'` policy turns that into a build failure.
///
/// Two obvious workarounds are worse. Writing `.value()` does not satisfy the
/// check either. Writing `.value_or({})` does, but `{}` cannot deduce
/// `optional::value_or`'s template parameter on every standard library — it
/// compiles under some and fails under others, which is exactly the kind of
/// difference that turns a green local build into a red CI run.
///
/// So the access is funnelled through one function, and the suppression is
/// stated once, here, where it can be justified in prose rather than
/// scattered across dozens of call sites. `value()` itself is safe: it throws
/// on a disengaged optional rather than reading uninitialised storage, so the
/// worst case is a test that fails loudly instead of one that reads garbage.
///
/// This lives under `tests/` and is never linked into a production target.
namespace aegis::test {

template <typename T>
[[nodiscard]] const T& checked(const std::optional<T>& optional_value) {
  // The caller's ASSERT_TRUE has already established engagement, and value()
  // throws rather than reading uninitialised storage if it somehow has not.
  // The suppression must sit on the line directly above the access.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return optional_value.value();
}

}  // namespace aegis::test

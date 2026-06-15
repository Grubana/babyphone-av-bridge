#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
namespace babycam { int placeholder(); }
TEST_CASE("toolchain works") { CHECK(babycam::placeholder() == 7); }

#include "doctest.h"

#include <vector>

#include "SLSManager.hpp"

// CSLSManager::check_invalid retires a reloaded manager's exited workers with
// sls_retire_exited. The loop it replaced cached end() before erasing, so when
// two workers had exited by the same pass the second one was deleted twice and
// erase() ran on end(): the reload-under-load E2E aborted with "double free or
// corruption".

TEST_CASE("worker retire: adjacent exited items are each retired exactly once")
{
    int a = 1, b = 2, c = 3, d = 4;
    std::vector<int *> items{&a, &b, &c, nullptr, &d};
    std::vector<int *> retired;
    sls_retire_exited(
        items, [](int &v) { return v != 3; }, [&](int *v) { retired.push_back(v); });
    CHECK(retired == std::vector<int *>{&a, &b, &d});
    CHECK(items == std::vector<int *>{&c});
}

TEST_CASE("worker retire: every item exiting in one pass empties the list")
{
    int a = 1, b = 2;
    std::vector<int *> items{&a, &b};
    std::vector<int *> retired;
    sls_retire_exited(
        items, [](int &) { return true; }, [&](int *v) { retired.push_back(v); });
    CHECK(retired == std::vector<int *>{&a, &b});
    CHECK(items.empty());
}

TEST_CASE("worker retire: live items stay and are never retired")
{
    int a = 1, b = 2;
    std::vector<int *> items{&a, &b};
    int retire_calls = 0;
    sls_retire_exited(
        items, [](int &) { return false; }, [&](int *) { ++retire_calls; });
    CHECK(retire_calls == 0);
    CHECK(items == std::vector<int *>{&a, &b});
}

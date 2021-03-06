#include <iostream>
#include <string>
#include <tuple>
#include <cassert>
#include "tests.h"
#include "verify.h"

/// The tuple we will use for our tests
typedef std::tuple<int, int, int> inttuple3;
typedef std::tuple<int, int>      inttuple2;
static inttuple3 * function_tuple  = NULL;
static inttuple2 * function_tuple2 = NULL;

void function_tests(int id)
{
    // test forward as tuple
    global_barrier->arrive(id);
    {
        verifier v;
        BEGIN_TX;
        function_tuple = new inttuple3(std::forward_as_tuple(1, 2, 3));
        v.insert_all<inttuple3>(function_tuple);
        delete(function_tuple);
        function_tuple = NULL;
        END_TX;
        v.check("forward as tuple (1)", id, 3, {1, 2, 3});
    }

    // test tie
    global_barrier->arrive(id);
    {
        verifier v;
        int arr[3];
        BEGIN_TX;
        function_tuple = new inttuple3(1, 2, 3);
        std::tie(arr[0], arr[1], arr[2]) = *function_tuple;
        v.insert(arr[0]);
        v.insert(arr[1]);
        v.insert(arr[2]);
        delete(function_tuple);
        function_tuple = NULL;
        END_TX;
        v.check("tie tuple (1)", id, 3, {1, 2, 3});
    }

}

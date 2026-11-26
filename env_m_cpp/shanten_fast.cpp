/*
 * shanten_fast.cpp  --  C++ accelerated shanten calculator for Mahjong.
 *
 * Build via _setup_env_m.py:
 *     python _setup_env_m.py build_ext --inplace
 *
 * Exposes:
 *   shanten_fast.calc_shanten(tiles: tuple[int,...], num_fixed: int) -> int
 *   shanten_fast.count_waiting(tiles: tuple[int,...], num_fixed: int) -> int
 *   shanten_fast.compute_tile_features(
 *       hand: list[int], num_fixed: int, remaining: list[int]
 *   ) -> tuple[list[float], list[float]]
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <tuple>
#include <vector>

namespace py = pybind11;

static constexpr int NUM_TILES = 34;
static constexpr int TERMINALS[13] = {
    0, 8, 9, 17, 18, 26, 27, 28, 29, 30, 31, 32, 33
};

// ---------------------------------------------------------------------------
// Core DFS shanten calculation
// ---------------------------------------------------------------------------

static void dfs(int tiles[NUM_TILES], int pos, int mentsu, int taatsu,
                int jantai, int num_fixed, int &best) {
    int total_m = num_fixed + mentsu;
    int eff_t = taatsu < (4 - total_m) ? taatsu : (4 - total_m);
    int sh = 8 - 2 * total_m - eff_t - jantai;
    if (sh < best)
        best = sh;
    if (best <= -1)
        return;

    int i = pos;
    while (i < NUM_TILES && tiles[i] == 0)
        ++i;
    if (i >= NUM_TILES)
        return;

    // Triplet (mentsu)
    if (tiles[i] >= 3) {
        tiles[i] -= 3;
        dfs(tiles, i, mentsu + 1, taatsu, jantai, num_fixed, best);
        tiles[i] += 3;
    }

    // Sequence (mentsu)
    if (i < 27 && (i % 9) <= 6 && tiles[i + 1] > 0 && tiles[i + 2] > 0) {
        tiles[i] -= 1;
        tiles[i + 1] -= 1;
        tiles[i + 2] -= 1;
        dfs(tiles, i, mentsu + 1, taatsu, jantai, num_fixed, best);
        tiles[i] += 1;
        tiles[i + 1] += 1;
        tiles[i + 2] += 1;
    }

    // Pair (jantai or taatsu)
    if (tiles[i] >= 2) {
        tiles[i] -= 2;
        if (jantai == 0)
            dfs(tiles, i, mentsu, taatsu, 1, num_fixed, best);
        dfs(tiles, i, mentsu, taatsu + 1, jantai, num_fixed, best);
        tiles[i] += 2;
    }

    // Adjacent pair (taatsu)
    if (i < 27 && (i % 9) <= 7 && tiles[i + 1] > 0) {
        tiles[i] -= 1;
        tiles[i + 1] -= 1;
        dfs(tiles, i, mentsu, taatsu + 1, jantai, num_fixed, best);
        tiles[i] += 1;
        tiles[i + 1] += 1;
    }

    // Gap pair (taatsu)
    if (i < 27 && (i % 9) <= 6 && tiles[i + 2] > 0) {
        tiles[i] -= 1;
        tiles[i + 2] -= 1;
        dfs(tiles, i, mentsu, taatsu + 1, jantai, num_fixed, best);
        tiles[i] += 1;
        tiles[i + 2] += 1;
    }

    // Skip this tile
    dfs(tiles, i + 1, mentsu, taatsu, jantai, num_fixed, best);
}

static int calc_regular_shanten(const int src[NUM_TILES], int num_fixed) {
    int buf[NUM_TILES];
    std::memcpy(buf, src, sizeof(buf));
    int best = 8 - 2 * num_fixed;
    dfs(buf, 0, 0, 0, 0, num_fixed, best);
    return best;
}

static int calc_seven_pairs(const int tiles[NUM_TILES]) {
    int pairs = 0;
    for (int i = 0; i < NUM_TILES; ++i)
        if (tiles[i] >= 2)
            ++pairs;
    return 6 - pairs;
}

static int calc_kokushi(const int tiles[NUM_TILES]) {
    int unique = 0, has_pair = 0;
    for (int k = 0; k < 13; ++k) {
        int idx = TERMINALS[k];
        if (tiles[idx] >= 1) ++unique;
        if (tiles[idx] >= 2) has_pair = 1;
    }
    return 13 - unique - has_pair;
}

static int full_shanten(const int tiles[NUM_TILES], int num_fixed) {
    int best = calc_regular_shanten(tiles, num_fixed);
    if (num_fixed == 0) {
        int sp = calc_seven_pairs(tiles);
        if (sp < best) best = sp;
        int ks = calc_kokushi(tiles);
        if (ks < best) best = ks;
    }
    return best;
}

static int count_waiting_impl(const int src[NUM_TILES], int num_fixed) {
    int tiles[NUM_TILES];
    std::memcpy(tiles, src, sizeof(tiles));
    int count = 0;
    for (int i = 0; i < NUM_TILES; ++i) {
        if (tiles[i] >= 4) continue;
        tiles[i] += 1;
        if (full_shanten(tiles, num_fixed) == -1)
            ++count;
        tiles[i] -= 1;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Python-facing functions
// ---------------------------------------------------------------------------

static int py_calc_shanten(py::tuple tiles_tuple, int num_fixed) {
    if (py::len(tiles_tuple) != NUM_TILES)
        throw py::value_error("tiles must have exactly 34 elements");
    int tiles[NUM_TILES];
    for (int i = 0; i < NUM_TILES; ++i)
        tiles[i] = tiles_tuple[i].cast<int>();
    return full_shanten(tiles, num_fixed);
}

static int py_count_waiting(py::tuple tiles_tuple, int num_fixed) {
    if (py::len(tiles_tuple) != NUM_TILES)
        throw py::value_error("tiles must have exactly 34 elements");
    int tiles[NUM_TILES];
    for (int i = 0; i < NUM_TILES; ++i)
        tiles[i] = tiles_tuple[i].cast<int>();
    return count_waiting_impl(tiles, num_fixed);
}

/*
 * Batched tile-feature computation.
 * Returns (importance[34], acceptance[34]).
 *
 * importance[i] = 1.0 if discarding tile i increases shanten (tile is needed).
 * acceptance[i] = remaining[i]/4.0 if drawing tile i decreases shanten (only
 *                 computed when shanten <= 2).
 */
static std::tuple<std::vector<double>, std::vector<double>>
py_compute_tile_features(py::list hand_list, int num_fixed,
                         py::list remaining_list) {
    if (py::len(hand_list) != NUM_TILES || py::len(remaining_list) != NUM_TILES)
        throw py::value_error("hand and remaining must have 34 elements");

    int hand[NUM_TILES];
    double remaining[NUM_TILES];
    for (int i = 0; i < NUM_TILES; ++i) {
        hand[i] = hand_list[i].cast<int>();
        remaining[i] = remaining_list[i].cast<double>();
    }

    int shanten = full_shanten(hand, num_fixed);

    std::vector<double> importance(NUM_TILES, 0.0);
    std::vector<double> acceptance(NUM_TILES, 0.0);

    if (shanten < 0)
        return {importance, acceptance};

    // Importance: which tiles increase shanten when discarded?
    for (int i = 0; i < NUM_TILES; ++i) {
        if (hand[i] <= 0) continue;
        hand[i] -= 1;
        int new_sh = full_shanten(hand, num_fixed);
        if (new_sh > shanten)
            importance[i] = 1.0;
        hand[i] += 1;
    }

    // Acceptance: which tiles decrease shanten when drawn?
    if (shanten <= 2) {
        for (int i = 0; i < NUM_TILES; ++i) {
            if (hand[i] >= 4 || remaining[i] <= 0.0) continue;
            hand[i] += 1;
            int new_sh = full_shanten(hand, num_fixed);
            if (new_sh < shanten)
                acceptance[i] = remaining[i] / 4.0;
            hand[i] -= 1;
        }
    }

    return {importance, acceptance};
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

PYBIND11_MODULE(shanten_fast, m) {
    m.doc() = "C++ accelerated shanten calculator for Chinese Standard Mahjong";

    m.def("calc_shanten", &py_calc_shanten,
          py::arg("tiles_tuple"), py::arg("num_fixed"),
          "Calculate full shanten number (regular + 7-pairs + kokushi).");

    m.def("count_waiting", &py_count_waiting,
          py::arg("tiles_tuple"), py::arg("num_fixed"),
          "Count how many distinct tiles would complete the hand (shanten -> -1).");

    m.def("compute_tile_features", &py_compute_tile_features,
          py::arg("hand"), py::arg("num_fixed"), py::arg("remaining"),
          "Batch compute tile importance and draw acceptance arrays.");
}

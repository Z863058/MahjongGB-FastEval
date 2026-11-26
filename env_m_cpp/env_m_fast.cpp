/*
 * env_m_fast.cpp  --  C++ accelerated Chinese Standard Mahjong environment.
 *
 * Build via _setup_env_m.py:
 *     python _setup_env_m.py build_ext --inplace
 *
 * Same surface as env_m.ChineseStandardMahjongEnv but faster; fan calculation
 * delegates to Python MahjongGB.
 *
 * Exposes:
 *   env_m_fast.ChineseStandardMahjongEnv(duplicate: bool = True)
 *   env_m_fast.Error, ResetNeeded, AlreadyClosed, AgentsNeeded
 *   Methods: reset(initdata=None), step(action), close()
 *   Properties: agents, requests, hands, packs, curPlayer, curTile, state,
 *       round, prevalentWind, player_num, duplicate, shownTiles, flowers,
 *       display, tileWall
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

// =====================================================================
// Tile representation: uint8_t 0-33 for normal tiles, 34-41 for flowers
// =====================================================================
// 0-8:   W1-W9
// 9-17:  B1-B9
// 18-26: T1-T9
// 27-30: F1-F4
// 31-33: J1-J3
// 34-41: H1-H8 (flowers, only in non-duplicate mode)

static constexpr uint8_t TILE_INVALID = 255;
static constexpr uint8_t TILE_H_START = 34;

static const char* TILE_STRINGS[] = {
    "W1","W2","W3","W4","W5","W6","W7","W8","W9",
    "B1","B2","B3","B4","B5","B6","B7","B8","B9",
    "T1","T2","T3","T4","T5","T6","T7","T8","T9",
    "F1","F2","F3","F4",
    "J1","J2","J3",
    "H1","H2","H3","H4","H5","H6","H7","H8"
};
static constexpr int NUM_TILE_STRINGS = 42;

static inline const char* tile_to_str(uint8_t t) {
    return TILE_STRINGS[t];
}

static inline std::string tile_to_string(uint8_t t) {
    return TILE_STRINGS[t];
}

static uint8_t str_to_tile(const std::string& s) {
    if (s.size() != 2) return TILE_INVALID;
    char c = s[0];
    int n = s[1] - '0';
    switch (c) {
        case 'W': if (n >= 1 && n <= 9) return (uint8_t)(n - 1);       break;
        case 'B': if (n >= 1 && n <= 9) return (uint8_t)(9 + n - 1);   break;
        case 'T': if (n >= 1 && n <= 9) return (uint8_t)(18 + n - 1);  break;
        case 'F': if (n >= 1 && n <= 4) return (uint8_t)(27 + n - 1);  break;
        case 'J': if (n >= 1 && n <= 3) return (uint8_t)(31 + n - 1);  break;
        case 'H': if (n >= 1 && n <= 8) return (uint8_t)(34 + n - 1);  break;
    }
    return TILE_INVALID;
}

static inline bool is_flower(uint8_t t) { return t >= TILE_H_START; }

static inline char tile_color(uint8_t t) {
    if (t < 9)  return 'W';
    if (t < 18) return 'B';
    if (t < 27) return 'T';
    if (t < 31) return 'F';
    if (t < 34) return 'J';
    return 'H';
}

static inline int tile_num(uint8_t t) {
    if (t < 9)  return t + 1;
    if (t < 18) return t - 9 + 1;
    if (t < 27) return t - 18 + 1;
    if (t < 31) return t - 27 + 1;
    if (t < 34) return t - 31 + 1;
    return t - 34 + 1;
}

// =====================================================================
// Pack (meld) representation
// =====================================================================
enum PackType : uint8_t { PACK_CHI = 0, PACK_PENG = 1, PACK_GANG = 2 };

static const char* pack_type_str(PackType t) {
    switch (t) {
        case PACK_CHI:  return "CHI";
        case PACK_PENG: return "PENG";
        case PACK_GANG: return "GANG";
    }
    return "?";
}

struct Pack {
    PackType type;
    uint8_t  tile;
    uint8_t  offer;
};

// =====================================================================
// Hand: small inline vector (max 14 tiles in hand)
// =====================================================================
struct Hand {
    uint8_t tiles[14];
    uint8_t size = 0;

    void clear() { size = 0; }
    void push(uint8_t t) { tiles[size++] = t; }

    int count(uint8_t t) const {
        int c = 0;
        for (uint8_t i = 0; i < size; i++) if (tiles[i] == t) c++;
        return c;
    }

    bool contains(uint8_t t) const {
        for (uint8_t i = 0; i < size; i++) if (tiles[i] == t) return true;
        return false;
    }

    bool remove_one(uint8_t t) {
        for (uint8_t i = 0; i < size; i++) {
            if (tiles[i] == t) {
                tiles[i] = tiles[--size];
                return true;
            }
        }
        return false;
    }

    void remove_n(uint8_t t, int n) {
        for (int k = 0; k < n; k++) remove_one(t);
    }
};

// =====================================================================
// Exception types exposed to Python (with proper inheritance)
// =====================================================================
struct EnvError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct EnvResetNeeded : EnvError {
    EnvResetNeeded() : EnvError("ResetNeeded") {}
};
struct EnvAlreadyClosed : EnvError {
    EnvAlreadyClosed() : EnvError("AlreadyClosed") {}
};
struct EnvAgentsNeeded : EnvError {
    EnvAgentsNeeded() : EnvError("AgentsNeeded") {}
};

// Internal exception for player rule violations (never escapes to Python)
struct GameError {
    int player;
    explicit GameError(int p) : player(p) {}
};

// =====================================================================
// Core Mahjong Environment (pure C++ state machine)
// =====================================================================
class MahjongEnvCore {
public:
    bool duplicate;
    bool closed = false;
    int  round_num = -2;        // -2 = not started, -1 = round ended (None equivalent)
    int  prevalentWind = 0;
    int  state = -1;
    int  curPlayer = 0;
    uint8_t curTile = TILE_INVALID;
    bool myWallLast = false;
    bool wallLast = false;
    bool isAboutKong = false;
    bool drawAboutKong = false;

    Hand hands[4];
    std::vector<Pack> packs[4];
    std::vector<uint8_t> flowers[4];
    uint8_t shown[34] = {};     // shownTiles — direct index by tile id (0-33)

    // Tile walls: in duplicate mode, 4 separate walls; otherwise 1
    std::vector<uint8_t> walls[4]; // duplicate=true: walls[0..3]
    std::vector<uint8_t> wall;     // duplicate=false: single wall

    std::array<std::string, 4> requests;

    // RNG
    std::mt19937 rng;
    bool seed_set = false;

    explicit MahjongEnvCore(bool dup) : duplicate(dup) {
        std::random_device rd;
        rng.seed(rd());
    }

    // ---- Tile wall operations ----
    uint8_t draw_tile(int player) {
        if (duplicate) {
            uint8_t t = walls[player].back();
            walls[player].pop_back();
            return t;
        }
        uint8_t t = wall.back();
        wall.pop_back();
        return t;
    }

    bool can_draw_tile(int player) const {
        if (duplicate) return !walls[player].empty();
        return !wall.empty();
    }

    // ---- Reset ----
    void reset(bool has_srand, int64_t srand_val,
               bool has_quan, int quan_val,
               bool has_walltiles, const std::string& walltiles_str,
               py::list py_agents) {
        if (closed) throw EnvAlreadyClosed();

        // Reset agents
        for (int i = 0; i < 4; i++) {
            py_agents[i].attr("reset")();
        }

        round_num = 0;

        if (has_srand) {
            rng.seed(static_cast<unsigned>(srand_val));
            seed_set = true;
        }

        if (has_quan) {
            prevalentWind = quan_val;
        } else {
            std::uniform_int_distribution<int> dist(0, 3);
            prevalentWind = dist(rng);
        }

        // Build tile wall
        std::vector<uint8_t> full_wall;
        if (has_walltiles) {
            std::istringstream iss(walltiles_str);
            std::string tok;
            while (iss >> tok) {
                uint8_t t = str_to_tile(tok);
                full_wall.push_back(t);
            }
        } else {
            full_wall.reserve(duplicate ? 136 : 144);
            for (int j = 0; j < 4; j++) {
                for (int i = 0; i < 9; i++)  full_wall.push_back((uint8_t)i);       // W1-W9
                for (int i = 9; i < 18; i++) full_wall.push_back((uint8_t)i);       // B1-B9
                for (int i = 18; i < 27; i++) full_wall.push_back((uint8_t)i);      // T1-T9
                for (int i = 27; i < 31; i++) full_wall.push_back((uint8_t)i);      // F1-F4
                for (int i = 31; i < 34; i++) full_wall.push_back((uint8_t)i);      // J1-J3
            }
            if (!duplicate) {
                for (int i = 0; i < 8; i++) full_wall.push_back((uint8_t)(34 + i)); // H1-H8
            }
            std::shuffle(full_wall.begin(), full_wall.end(), rng);
        }

        if (duplicate) {
            for (int i = 0; i < 4; i++) {
                walls[i].assign(full_wall.begin() + i * 34,
                                full_wall.begin() + (i + 1) * 34);
            }
        } else {
            wall = std::move(full_wall);
        }

        std::memset(shown, 0, sizeof(shown));
        state = -1;

        for (int sw = 0; sw < 4; sw++) {
            requests[sw] = "0 " + std::to_string(sw) + " " + std::to_string(prevalentWind);
        }
    }

    // ---- Deal ----
    void deal() {
        for (int i = 0; i < 4; i++) {
            hands[i].clear();
            flowers[i].clear();
            packs[i].clear();
        }

        for (int i = 0; i < 4; i++) {
            while (hands[i].size < 13) {
                uint8_t t = draw_tile(i);
                if (is_flower(t)) {
                    flowers[i].push_back(t);
                } else {
                    hands[i].push(t);
                }
            }
        }

        // Build flower count string and flower tiles string
        std::string fc;
        for (int i = 0; i < 4; i++) {
            if (i > 0) fc += ' ';
            fc += std::to_string(flowers[i].size());
        }

        std::string ft;
        for (int i = 0; i < 4; i++) {
            for (uint8_t f : flowers[i]) {
                if (!ft.empty()) ft += ' ';
                ft += tile_to_str(f);
            }
        }

        // Build requests: "1 <flowerCounts> <hand tiles> <all flower tiles>"
        for (int p = 0; p < 4; p++) {
            std::string req = "1 " + fc;
            for (uint8_t j = 0; j < hands[p].size; j++) {
                req += ' ';
                req += tile_to_str(hands[p].tiles[j]);
            }
            if (!ft.empty()) {
                req += ' ';
                req += ft;
            }
            requests[p] = std::move(req);
        }

        state = 0;
        curPlayer = 0;
        drawAboutKong = false;
    }

    // ---- Draw ----
    void draw(int player) {
        uint8_t t = draw_tile(player);
        myWallLast = !can_draw_tile(player);
        wallLast   = !can_draw_tile((player + 1) % 4);
        isAboutKong = drawAboutKong;
        drawAboutKong = false;

        if (is_flower(t)) {
            state = 0;
            flowers[player].push_back(t);
            std::string msg = "3 " + std::to_string(player) + " BUHUA " + tile_to_str(t);
            for (int i = 0; i < 4; i++) requests[i] = msg;
        } else {
            state = 1;
            curTile = t;
            std::string draw_msg = "3 " + std::to_string(player) + " DRAW";
            std::string self_msg = "2 " + tile_to_string(t);
            for (int i = 0; i < 4; i++) {
                requests[i] = (i == player) ? self_msg : draw_msg;
            }
        }
    }

    // ---- Discard ----
    void discard(int player, uint8_t tile) {
        hands[player].push(curTile);
        if (!hands[player].contains(tile)) throw GameError(player);
        hands[player].remove_one(tile);
        shown[tile]++;
        curTile = tile;
        state = 2;
        std::string msg = "3 " + std::to_string(player) + " PLAY " + tile_to_str(tile);
        for (int i = 0; i < 4; i++) requests[i] = msg;
    }

    // ---- Kong (concealed / exposed) ----
    void kong(int player, uint8_t tile) {
        hands[player].push(curTile);
        if (hands[player].count(tile) < 4) throw GameError(player);
        hands[player].remove_n(tile, 4);
        packs[player].push_back({PACK_GANG, tile, (uint8_t)((player + 4 - curPlayer) % 4)});
        shown[tile] = 4;
        state = 0;
        curPlayer = player;
        drawAboutKong = true;
        std::string msg = "3 " + std::to_string(player) + " GANG " + tile_to_str(tile);
        for (int i = 0; i < 4; i++) requests[i] = msg;
    }

    // ---- Pung ----
    void pung(int player, uint8_t tile, uint8_t discard_tile) {
        hands[player].push(curTile);
        if (hands[player].count(tile) < 3) throw GameError(player);
        hands[player].remove_n(tile, 3);
        packs[player].push_back({PACK_PENG, tile, (uint8_t)((player + 4 - curPlayer) % 4)});
        shown[tile] += 2;
        if (!hands[player].contains(discard_tile)) throw GameError(player);
        hands[player].remove_one(discard_tile);
        shown[discard_tile]++;
        state = 2;
        curPlayer = player;
        wallLast = !can_draw_tile((player + 1) % 4);
        curTile = discard_tile;
        std::string msg = "3 " + std::to_string(player) + " PENG " + tile_to_str(discard_tile);
        for (int i = 0; i < 4; i++) requests[i] = msg;
    }

    // ---- Chow ----
    void chow(int player, uint8_t mid_tile, uint8_t discard_tile) {
        hands[player].push(curTile);
        shown[curTile]--;
        char color = tile_color(mid_tile);
        int num = tile_num(mid_tile);
        for (int d = -1; d <= 1; d++) {
            uint8_t t;
            if (color == 'W')      t = (uint8_t)(num - 1 + d);
            else if (color == 'B') t = (uint8_t)(9 + num - 1 + d);
            else                   t = (uint8_t)(18 + num - 1 + d);  // 'T'
            if (!hands[player].contains(t)) throw GameError(player);
            hands[player].remove_one(t);
            shown[t]++;
        }
        int offer = tile_num(curTile) - num + 2;
        packs[player].push_back({PACK_CHI, mid_tile, (uint8_t)offer});
        if (!hands[player].contains(discard_tile)) throw GameError(player);
        hands[player].remove_one(discard_tile);
        shown[discard_tile]++;
        state = 2;
        curPlayer = player;
        wallLast = !can_draw_tile((player + 1) % 4);
        curTile = discard_tile;
        std::string msg = "3 " + std::to_string(player) + " CHI " +
                          tile_to_str(mid_tile) + " " + tile_to_str(discard_tile);
        for (int i = 0; i < 4; i++) requests[i] = msg;
    }

    // ---- Promote Kong (Bu Gang) ----
    void promote_kong(int player, uint8_t tile) {
        hands[player].push(curTile);
        int idx = -1;
        for (int i = 0; i < (int)packs[player].size(); i++) {
            if (packs[player][i].type == PACK_PENG && packs[player][i].tile == tile) {
                idx = i;
            }
        }
        if (idx < 0) throw GameError(player);
        if (!hands[player].contains(tile)) throw GameError(player);
        hands[player].remove_one(tile);
        uint8_t offer = packs[player][idx].offer;
        packs[player][idx] = {PACK_GANG, tile, offer};
        shown[tile] = 4;
        state = 3;
        curPlayer = player;
        curTile = tile;
        drawAboutKong = true;
        std::string msg = "3 " + std::to_string(player) + " BUGANG " + tile_to_str(tile);
        for (int i = 0; i < 4; i++) requests[i] = msg;
    }

    // ---- Check Mahjong (calls Python MahjongFanCalculator) ----
    // Returns score tuple or throws GameError
    std::array<int, 4> check_mahjong(int player, bool isSelfDrawn, bool is_AboutKong) {
        // Build pack tuples for Python call
        py::list py_packs;
        for (auto& pk : packs[player]) {
            py_packs.append(py::make_tuple(
                std::string(pack_type_str(pk.type)),
                tile_to_string(pk.tile),
                (int)pk.offer
            ));
        }

        // Build hand tuple
        py::list py_hand;
        for (uint8_t i = 0; i < hands[player].size; i++) {
            py_hand.append(tile_to_string(hands[player].tiles[i]));
        }

        std::string win_tile_str = tile_to_string(curTile);
        int flower_count = (int)flowers[player].size();
        bool is4th = (shown[curTile] == 4);

        try {
            py::module_ mj = py::module_::import("MahjongGB");
            py::object calc = mj.attr("MahjongFanCalculator");

            py::object result = calc(
                py::arg("pack") = py::tuple(py_packs),
                py::arg("hand") = py::tuple(py_hand),
                py::arg("winTile") = win_tile_str,
                py::arg("flowerCount") = flower_count,
                py::arg("isSelfDrawn") = isSelfDrawn,
                py::arg("is4thTile") = is4th,
                py::arg("isAboutKong") = is_AboutKong,
                py::arg("isWallLast") = wallLast,
                py::arg("seatWind") = player,
                py::arg("prevalentWind") = prevalentWind,
                py::arg("verbose") = true
            );

            int fanCnt = 0;
            for (auto item : result) {
                py::tuple t = item.cast<py::tuple>();
                int fanPoint = t[0].cast<int>();
                int cnt = t[1].cast<int>();
                fanCnt += fanPoint * cnt;
            }

            if (fanCnt < 8) throw GameError(player);

            std::array<int, 4> score;
            if (isSelfDrawn) {
                score.fill(-(8 + fanCnt));
                score[player] = (8 + fanCnt) * 3;
            } else {
                score.fill(-8);
                score[player] = 8 * 3 + fanCnt;
                score[curPlayer] -= fanCnt;
            }
            round_num = -1;
            return score;
        } catch (const GameError&) {
            throw;
        } catch (...) {
            throw GameError(player);
        }
    }

    // ---- Main step logic ----
    // Returns: empty optional (None) if game continues, or score array
    struct StepResult {
        bool done;
        std::array<int, 4> scores;
    };

    StepResult step(py::list py_agents) {
        if (closed) throw EnvAlreadyClosed();
        if (round_num < 0) throw EnvResetNeeded();

        // Collect responses from agents
        std::array<std::string, 4> responses;
        for (int i = 0; i < 4; i++) {
            py::object resp = py_agents[i].attr("step")(requests[i]);
            if (!py::isinstance<py::str>(resp)) throw GameError(i);
            responses[i] = resp.cast<std::string>();
        }
        round_num++;

        try {
            if (state == -1) {
                for (int i = 0; i < 4; i++)
                    if (responses[i] != "PASS") throw GameError(i);
                deal();
            }
            else if (state == 0) {
                for (int i = 0; i < 4; i++)
                    if (responses[i] != "PASS") throw GameError(i);
                draw(curPlayer);
            }
            else if (state == 1) {
                for (int i = 0; i < 4; i++)
                    if (i != curPlayer && responses[i] != "PASS") throw GameError(i);

                auto tokens = split(responses[curPlayer]);
                if (tokens[0] == "HU") {
                    shown[curTile]++;
                    auto sc = check_mahjong(curPlayer, true, isAboutKong);
                    return {true, sc};
                }
                else if (tokens[0] == "PLAY") {
                    if (tokens.size() != 2) throw GameError(curPlayer);
                    uint8_t t = str_to_tile(tokens[1]);
                    if (t == TILE_INVALID) throw GameError(curPlayer);
                    discard(curPlayer, t);
                }
                else if (tokens[0] == "GANG" && !myWallLast && !wallLast) {
                    if (tokens.size() != 2) throw GameError(curPlayer);
                    uint8_t t = str_to_tile(tokens[1]);
                    if (t == TILE_INVALID) throw GameError(curPlayer);
                    kong(curPlayer, t);
                }
                else if (tokens[0] == "BUGANG" && !myWallLast && !wallLast) {
                    if (tokens.size() != 2) throw GameError(curPlayer);
                    uint8_t t = str_to_tile(tokens[1]);
                    if (t == TILE_INVALID) throw GameError(curPlayer);
                    promote_kong(curPlayer, t);
                }
                else {
                    throw GameError(curPlayer);
                }
                isAboutKong = false;
            }
            else if (state == 2) {
                if (responses[curPlayer] != "PASS") throw GameError(curPlayer);

                // Lazy split
                std::array<std::vector<std::string>, 4> t;
                for (int i = 0; i < 4; i++) {
                    t[i] = (responses[i] == "PASS") ?
                            std::vector<std::string>{"PASS"} : split(responses[i]);
                }

                // Priority: HU
                for (int j = 0; j < 4; j++) {
                    int i = (curPlayer + j) % 4;
                    if (responses[i] == "HU") {
                        auto sc = check_mahjong(i, false, false);
                        return {true, sc};
                    }
                }

                // Priority: KONG / PUNG
                bool handled = false;
                for (int i = 0; i < 4; i++) {
                    if (responses[i] == "GANG" && can_draw_tile(i) && !wallLast) {
                        kong(i, curTile);
                        handled = true;
                        break;
                    }
                    if (t[i][0] == "PENG" && !wallLast && t[i].size() == 2) {
                        uint8_t dt = str_to_tile(t[i][1]);
                        if (dt == TILE_INVALID) throw GameError(i);
                        pung(i, curTile, dt);
                        handled = true;
                        break;
                    }
                }

                if (!handled) {
                    int next = (curPlayer + 1) % 4;
                    if (t[next][0] == "CHI" && !wallLast && t[next].size() == 3) {
                        uint8_t mt = str_to_tile(t[next][1]);
                        uint8_t dt = str_to_tile(t[next][2]);
                        if (mt == TILE_INVALID || dt == TILE_INVALID) throw GameError(next);
                        chow(next, mt, dt);
                    } else {
                        for (int i = 0; i < 4; i++)
                            if (responses[i] != "PASS") throw GameError(i);
                        if (wallLast) {
                            round_num = -1;
                            return {true, {0, 0, 0, 0}};
                        }
                        curPlayer = (curPlayer + 1) % 4;
                        draw(curPlayer);
                    }
                }
            }
            else if (state == 3) {
                if (responses[curPlayer] != "PASS") throw GameError(curPlayer);
                for (int j = 0; j < 4; j++) {
                    int i = (curPlayer + j) % 4;
                    if (responses[i] == "HU") {
                        auto sc = check_mahjong(i, false, true);
                        return {true, sc};
                    }
                }
                for (int i = 0; i < 4; i++)
                    if (responses[i] != "PASS") throw GameError(i);
                draw(curPlayer);
            }
        } catch (const GameError& e) {
            std::array<int, 4> score = {10, 10, 10, 10};
            score[e.player] = -30;
            round_num = -1;
            return {true, score};
        }

        return {false, {}};
    }

private:
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) tokens.push_back(std::move(tok));
        return tokens;
    }
};


// =====================================================================
// pybind11 Python wrapper
// =====================================================================
class PyMahjongEnv {
    MahjongEnvCore core;
    py::list agents_;
    bool has_agents_ = false;

public:
    explicit PyMahjongEnv(bool duplicate = true) : core(duplicate) {}

    // --- agents property ---
    void set_agents(py::list a) {
        if (py::len(a) != 4) throw EnvError("agents must be a list of 4 objects");
        agents_ = a;
        has_agents_ = true;
    }
    py::list get_agents() const { return agents_; }

    // --- reset ---
    void reset(py::object initdata_obj) {
        if (core.closed) throw EnvAlreadyClosed();
        if (!has_agents_) throw EnvAgentsNeeded();

        py::dict initdata;
        if (!initdata_obj.is_none() && py::isinstance<py::dict>(initdata_obj) && py::len(initdata_obj) > 0) {
            initdata = initdata_obj.cast<py::dict>();
        }

        bool has_srand = initdata.contains("srand");
        int64_t srand_val = has_srand ? initdata["srand"].cast<int64_t>() : 0;

        bool has_quan = initdata.contains("quan");
        int quan_val = has_quan ? initdata["quan"].cast<int>() : 0;

        bool has_wt = initdata.contains("walltiles");
        std::string wt_str = has_wt ? initdata["walltiles"].cast<std::string>() : "";

        core.reset(has_srand, srand_val, has_quan, quan_val, has_wt, wt_str, agents_);
    }

    // --- step ---
    py::object step() {
        if (!has_agents_) throw EnvAgentsNeeded();
        auto result = core.step(agents_);
        if (result.done) {
            return py::make_tuple(result.scores[0], result.scores[1],
                                  result.scores[2], result.scores[3]);
        }
        return py::none();
    }

    // --- close ---
    void close() { core.closed = true; }

    // --- Property: requests ---
    py::list get_requests() const {
        py::list r;
        for (int i = 0; i < 4; i++) r.append(core.requests[i]);
        return r;
    }

    // --- Property: hands (list of 4 lists of tile strings) ---
    py::list get_hands() const {
        py::list outer;
        for (int p = 0; p < 4; p++) {
            py::list inner;
            for (uint8_t j = 0; j < core.hands[p].size; j++) {
                inner.append(tile_to_string(core.hands[p].tiles[j]));
            }
            outer.append(inner);
        }
        return outer;
    }

    // --- Property: packs (list of 4 lists of (type_str, tile_str, offer_int) tuples) ---
    py::list get_packs() const {
        py::list outer;
        for (int p = 0; p < 4; p++) {
            py::list inner;
            for (auto& pk : core.packs[p]) {
                inner.append(py::make_tuple(
                    std::string(pack_type_str(pk.type)),
                    tile_to_string(pk.tile),
                    (int)pk.offer
                ));
            }
            outer.append(inner);
        }
        return outer;
    }

    // --- Property: curPlayer ---
    int get_curPlayer() const { return core.curPlayer; }

    // --- Property: curTile (string) ---
    py::object get_curTile() const {
        if (core.curTile == TILE_INVALID) return py::none();
        return py::cast(tile_to_string(core.curTile));
    }

    // --- Property: state ---
    int get_state() const { return core.state; }

    // --- Property: round (int or None) ---
    py::object get_round() const {
        if (core.round_num < 0) return py::none();
        return py::cast(core.round_num);
    }

    // --- Property: prevalentWind ---
    int get_prevalentWind() const { return core.prevalentWind; }

    // --- Property: player_num ---
    int get_player_num() const { return 4; }

    // --- Property: duplicate ---
    bool get_duplicate() const { return core.duplicate; }

    // --- Property: shownTiles (dict-like, for compatibility if anyone reads it) ---
    py::dict get_shownTiles() const {
        py::dict d;
        for (int i = 0; i < 34; i++) {
            if (core.shown[i] > 0) {
                d[py::cast(tile_to_string((uint8_t)i))] = (int)core.shown[i];
            }
        }
        return d;
    }

    // --- Property: flowers ---
    py::list get_flowers() const {
        py::list outer;
        for (int p = 0; p < 4; p++) {
            py::list inner;
            for (uint8_t f : core.flowers[p]) {
                inner.append(tile_to_string(f));
            }
            outer.append(inner);
        }
        return outer;
    }

    // --- Property: display (empty list for compatibility) ---
    py::list get_display() const { return py::list(); }

    // --- Property: tileWall ---
    py::object get_tileWall() const {
        if (core.duplicate) {
            py::list outer;
            for (int i = 0; i < 4; i++) {
                py::list inner;
                for (uint8_t t : core.walls[i]) inner.append(tile_to_string(t));
                outer.append(inner);
            }
            return outer;
        } else {
            py::list lst;
            for (uint8_t t : core.wall) lst.append(tile_to_string(t));
            return lst;
        }
    }
};


PYBIND11_MODULE(env_m_fast, m) {
    m.doc() = "C++ accelerated Chinese Standard Mahjong environment";

    // Register exception classes with proper inheritance (Error -> Exception)
    static auto ex_Error = py::register_exception<EnvError>(m, "Error");
    py::register_exception<EnvResetNeeded>(m, "ResetNeeded", ex_Error.ptr());
    py::register_exception<EnvAlreadyClosed>(m, "AlreadyClosed", ex_Error.ptr());
    py::register_exception<EnvAgentsNeeded>(m, "AgentsNeeded", ex_Error.ptr());

    py::class_<PyMahjongEnv>(m, "ChineseStandardMahjongEnv")
        .def(py::init<bool>(), py::arg("duplicate") = true)
        .def("reset", &PyMahjongEnv::reset, py::arg("initdata") = py::none())
        .def("step", &PyMahjongEnv::step)
        .def("close", &PyMahjongEnv::close)
        .def_property("agents", &PyMahjongEnv::get_agents, &PyMahjongEnv::set_agents)
        .def_property_readonly("requests", &PyMahjongEnv::get_requests)
        .def_property_readonly("hands", &PyMahjongEnv::get_hands)
        .def_property_readonly("packs", &PyMahjongEnv::get_packs)
        .def_property_readonly("curPlayer", &PyMahjongEnv::get_curPlayer)
        .def_property_readonly("curTile", &PyMahjongEnv::get_curTile)
        .def_property_readonly("state", &PyMahjongEnv::get_state)
        .def_property_readonly("round", &PyMahjongEnv::get_round)
        .def_property_readonly("prevalentWind", &PyMahjongEnv::get_prevalentWind)
        .def_property_readonly("player_num", &PyMahjongEnv::get_player_num)
        .def_property_readonly("duplicate", &PyMahjongEnv::get_duplicate)
        .def_property_readonly("shownTiles", &PyMahjongEnv::get_shownTiles)
        .def_property_readonly("flowers", &PyMahjongEnv::get_flowers)
        .def_property_readonly("display", &PyMahjongEnv::get_display)
        .def_property_readonly("tileWall", &PyMahjongEnv::get_tileWall);
}

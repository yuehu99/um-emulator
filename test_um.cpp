// um.cpp — Universal Machine (UM) Emulator
// Build: g++ -O2 -std=c++17 -Wall -Wextra -o um um.cpp
// Usage: ./um <program.um>
#include <bits/stdc++.h>
using namespace std;

using u32 = uint32_t;
using u64 = uint64_t;

struct UM {
    // ---- 机器状态 ----
    u32 R[8]{};                        // 8 个 32 位寄存器（上电全 0）
    vector<vector<u32>> arrays;        // 数组池：id == 下标；0 号数组存放程序
    vector<uint8_t> active;            // active[id] != 0 表示该数组处于已分配&可用
    vector<u32> free_ids;              // 已释放 id 的回收池（复用）
    u32 pc = 0;                        // 程序计数器（索引 arrays[0]）

    // ---- 工具：失败即退出 ----
    [[noreturn]] void fail(const string& msg) {
        fprintf(stderr, "UM Fail: %s\n", msg.c_str());
        exit(1);
    }

    // ---- 从 .um 文件读入程序到 0 号数组（每 4 字节大端组成 1 条 32 位指令）----
    void load_program_file(const string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) fail("cannot open program file: " + path);

        vector<u32> prog;
        unsigned char b[4];
        size_t total = 0;
        while (true) {
            size_t got = fread(b, 1, 4, f);
            if (got == 0) break;
            if (got != 4) { fclose(f); fail("program file size not divisible by 4"); }
            u32 w = (u32(b[0])<<24) | (u32(b[1])<<16) | (u32(b[2])<<8) | u32(b[3]); // 大端
            prog.push_back(w);
            total += 4;
        }
        fclose(f);

        arrays.clear(); active.clear(); free_ids.clear();
        arrays.push_back(std::move(prog)); // id 0 = 程序数组
        active.push_back(1);               // 0 号数组永远处于 active 状态（不能释放）
        pc = 0;
        memset(R, 0, sizeof(R));
    }

    // ---- 分配/释放/检查 ----
    u32 alloc_array(u32 words) {
        // 新建一个长度为 words 的数组（元素初始化为 0），返回 id（非 0）
        size_t len = (size_t)words;
        u32 id;
        if (!free_ids.empty()) {
            id = free_ids.back(); free_ids.pop_back();
            size_t uid = (size_t)id;
            if (uid >= arrays.size()) fail("internal: recycled id out of range");
            arrays[uid].assign(len, 0);
            active[uid] = 1;
        } else {
            id = (u32)arrays.size();
            arrays.push_back(vector<u32>(len, 0));
            active.push_back(1);
        }
        if (id == 0) fail("internal: allocated id 0 (forbidden)");
        return id;
    }

    void free_array(u32 id) {
        if (id == 0) fail("attempt to deallocate array 0");
        size_t uid = (size_t)id;
        if (uid >= arrays.size() || !active[uid]) fail("deallocate a non-active array");
        active[uid] = 0;
        arrays[uid].clear();
        arrays[uid].shrink_to_fit();
        free_ids.push_back(id);
    }

    void check_active(u32 id) {
        size_t uid = (size_t)id;
        if (uid >= arrays.size() || !active[uid]) fail("accessing non-active array id=" + to_string(id));
    }
    void check_bounds(u32 id, u32 off) {
        size_t uid = (size_t)id, uoff = (size_t)off;
        if (uid >= arrays.size() || !active[uid]) fail("accessing non-active array id=" + to_string(id));
        if (uoff >= arrays[uid].size()) fail("array index out of bounds");
    }

    // ---- 位段解析工具 ----
    static inline u32 OPC(u32 w){ return w >> 28; }        // opcode = bits 28..31
    static inline u32 Astd(u32 w){ return (w >> 6) & 7u; } // 标准格式 A = bits 6..8
    static inline u32 Bstd(u32 w){ return (w >> 3) & 7u; } // 标准格式 B = bits 3..5
    static inline u32 Cstd(u32 w){ return (w >> 0) & 7u; } // 标准格式 C = bits 0..2

    // ---- 主执行循环：取指 -> 解码 -> 执行 ->（除 Load Program 外）PC++ ----
    void run() {
        for (;;) {
            // 每个周期起始：PC 必须在 0 号数组范围内
            if (pc >= arrays[0].size()) fail("program counter outside array 0 capacity");

            u32 inst = arrays[0][pc];
            u32 op   = OPC(inst);
            bool advance_pc = true; // 默认执行后 pc++（Load Program 例外）

            if (op == 13) {
                // 特殊指令：Load Immediate
                // [31..28]=opcode(13), [27..25]=A, [24..0]=value(25 bits)
                u32 A = (inst >> 25) & 7u;
                u32 val = inst & 0x1FFFFFFu;
                R[A] = val;
            } else {
                // 标准指令：A,B,C 都是 3 位寄存器编号
                u32 A = Astd(inst), B = Bstd(inst), C = Cstd(inst);
                switch (op) {
                    case 0: // Conditional Move: if R[C] != 0 then R[A] = R[B]
                        if (R[C] != 0) R[A] = R[B];
                        break;

                    case 1: { // Array Index: R[A] = arrays[R[B]][R[C]]
                        u32 id = R[B], off = R[C];
                        check_active(id); check_bounds(id, off);
                        R[A] = arrays[(size_t)id][(size_t)off];
                    } break;

                    case 2: { // Array Update: arrays[R[A]][R[B]] = R[C]
                        u32 id = R[A], off = R[B];
                        check_active(id); check_bounds(id, off);
                        arrays[(size_t)id][(size_t)off] = R[C];
                    } break;

                    case 3: // Addition: R[A] = (R[B] + R[C]) mod 2^32
                        R[A] = (u32)((u64)R[B] + (u64)R[C]);
                        break;

                    case 4: // Multiplication: R[A] = (R[B] * R[C]) mod 2^32
                        R[A] = (u32)((u64)R[B] * (u64)R[C]);
                        break;

                    case 5: // Division (unsigned): R[A] = R[B] / R[C]
                        if (R[C] == 0) fail("division by zero");
                        R[A] = (u32)((u64)R[B] / (u64)R[C]);
                        break;

                    case 6: // Nand: R[A] = ~(R[B] & R[C])
                        R[A] = ~(R[B] & R[C]);
                        break;

                    case 7: // Halt
                        return;

                    case 8: { // Allocation: create array of R[C] words (zeroed); id -> R[B]
                        u32 words = R[C];
                        u32 id = alloc_array(words);
                        R[B] = id;
                    } break;

                    case 9: // Deallocation: free array id in R[C]
                        free_array(R[C]);
                        break;

                    case 10: { // Output: print byte in R[C] (0..255)
                        u32 v = R[C];
                        if (v > 255u) fail("output value > 255");
                        int ch = (int)(v & 0xFFu);
                        if (putchar(ch) == EOF) fail("output error");
                        // 按需刷新：不强制 fflush，以提升性能
                    } break;

                    case 11: { // Input: read byte -> R[C]; EOF => all 1's
                        int ch = getchar();
                        if (ch == EOF) R[C] = 0xFFFFFFFFu;
                        else           R[C] = (u32)(ch & 0xFF);
                    } break;

                    case 12: { // Load Program: duplicate arrays[R[B]] into arrays[0]; pc = R[C]
                        u32 id = R[B];
                        check_active(id);            // 必须是 active 的数组
                        // 规范要求 duplicate（复制），即便 id==0 也要复制再替换
                        vector<u32> dup = arrays[(size_t)id];
                        arrays[0] = std::move(dup);
                        pc = R[C];                   // 跳转到新程序的 offset
                        advance_pc = false;          // 不再 pc++
                    } break;

                    default:
                        fail("invalid opcode");
                }
            }

            if (advance_pc) ++pc;
        }
    }
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <program.um>\n", argv[0]);
        return 1;
    }
    UM um;
    um.load_program_file(argv[1]); // 把 .um 文件按大端读入到 0 号数组
    um.run();                      // 进入取指-执行循环
    return 0;
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <iostream> // 如果用到
using namespace std;

using u32 = uint32_t;

struct UM {

    u32 R[8] = {0}; // Registers
    vector<vector<u32>> arrays; // Memory segments
    vector<u32> free_ids; // Reusable segment IDs
    u32 pc = 0; // Program counter
    bool halted = false; // Halt flag
    vector<uint8_t> active; // Active

    // Error handling
    [[noreturn]] void fail(const string& msg) {
        fprintf(stderr, "UM Fail: %s\n", msg.c_str());
        exit(1);
    }

    // Load program from file
    void load_program(const string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) fail("cannot open program file: " + path);

        vector<u32> prog;
        unsigned char b[4];
        while (true) {
            size_t got = fread(b, 1, 4, f);
            if (got == 0) break;
            if (got != 4) { fclose(f); fail("program file size not divisible by 4"); }
            u32 w = (static_cast<u32>(b[0])<<24) | (static_cast<u32>(b[1])<<16)
                    | (static_cast<u32>(b[2])<<8)  |  static_cast<u32>(b[3]); // Big-endian
            prog.push_back(w);
        }
        fclose(f);

        arrays.clear();
        active.clear();
        free_ids.clear();
        arrays.push_back(std::move(prog));
        active.push_back(1);
        pc = 0;
        halted = false;
        memset(R, 0, sizeof(R));
    }

    // Allocate a new array segment of given size and returns the ID of the allocated segment
    u32 alloc_array(u32 size) {
        size_t len = (size_t)size;     // allow len == 0
        u32 id;

        if (!free_ids.empty()) {
            id = free_ids.back();
            free_ids.pop_back();
            size_t idx = (size_t)id;
            if (idx >= arrays.size()) fail("internal: recycled id out of range");
            if (active[idx])          fail("internal: recycled id still active");
            try {
                arrays[idx].assign(len, 0);  
                active[idx] = 1;
            } catch (const std::bad_alloc&) {
                fail("Out of memory while resizing array");
            }
        } else {
            id = (u32)arrays.size();         
            try {
                arrays.emplace_back(len, 0); 
                active.push_back(1);
            } catch (const std::bad_alloc&) {
                fail("Out of memory while allocating array");
            }
        }
        if (id == 0) fail("internal: allocated id 0 (forbidden)");
        return id;
    }


    // decoder
    //[ opcode:4 ][ ....unused/reserved.... ][ A:3 ][ B:3 ][ C:3 ]
    // 31       28 27                       9 8   6  5   3  2   0
    static inline u32 OP(u32 w) { return w >> 28 ; }
    static inline u32 A(u32 w) { return (w >> 6) & 7u; }
    static inline u32 B(u32 w) { return (w >> 3) & 7u; }
    static inline u32 C(u32 w) { return (w >> 0) & 7u; }

    void check_active(u32 id) {
        size_t uid = (size_t)id;
        if (uid >= arrays.size() || !active[uid]) fail("accessing non-active array id=" + to_string(id));
    }

    void  check_bounds(u32 id, u32 off) {
        size_t uid = (size_t)id, uoff = (size_t)off;
        if (uid >= arrays.size() || !active[uid]) fail("accessing non-active array id=" + to_string(id));
        if (uoff >= arrays[uid].size()) fail("array index out of bounds");
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

    void run() {
        while (!halted) {
            if (pc >= arrays[0].size()) fail("PC out of bounds");
            u32 instr = arrays[0][pc];
            u32 op = OP(instr);
            bool advance_pc = true;
            if (op == 13){
                // Load Value
                u32 a = (instr >> 25) & 7u;
                u32 val = instr & 0x1FFFFFFu;
                R[a] = val;
            } else {
                u32 a = A(instr), b = B(instr), c = C(instr);
                switch (op) {
                    case 0: // Conditional Move
                        if (R[c] != 0) R[a] = R[b];
                        break;
                    case 1: // Array Index
                        check_active(R[b]);
                        check_bounds(R[b], R[c]);
                        R[a] = arrays[R[b]][R[c]];
                        break;
                    case 2: // Array Amendment
                        check_active(R[a]);
                        check_bounds(R[a], R[b]);
                        arrays[R[a]][R[b]] = R[c];
                        break;
                    case 3: R[a] = R[b] + R[c]; break; // Addition
                    case 4: R[a] = R[b] * R[c]; break; // Multiplication
                    case 5: // Division
                        if (R[c] == 0) fail("Division by zero");
                        R[a] = (u32)(R[b] / R[c]);
                        break;
                    case 6: // Not-And
                        R[a] = ~(R[b] & R[c]);
                        break;
                    case 7: // Halt
                        halted = true;
                        break;
                    case 8: { // Allocation
                        u32 words = R[c];
                        u32 id = alloc_array(words);
                        R[b] = id;
                    } break;
                    case 9: // Deallocation
                        free_array(R[c]);
                        break;
                    case 10: // Output: print R[C] as a character
                        if (R[c] > 255u) fail("Output value out of range");
                        if (putchar(static_cast<unsigned char>(R[c])) == EOF) fail("output error");
                        break;

                    case 11: { // Input
                        int ch = getchar();
                        if (ch == EOF) {
                            R[c] = 0xFFFFFFFFu;
                        } else {
                            R[c] = static_cast<u32>(static_cast<unsigned char>(ch));
                        }
                    } break;
                    case 12: { // Load Program Segment
                        u32 id = R[b], new_pc = R[c];
                        if (id != 0) {
                            check_active(id);
                            arrays[0] = arrays[id];   
                        }
                        pc = new_pc;                  
                        advance_pc = false;          
                    } break;

                    default:
                        fail("Unknown operation code: " + to_string(op));  
                }
            }

            if (advance_pc) pc++;
        }
    } 
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <program.um>\n", argv[0]);
        return 1;
    }
    UM um;
    um.load_program(argv[1]); 
    um.run();                      
    return 0;
}

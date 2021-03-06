﻿
#include "mcore/mcore.h"
#include "elfloadimage.hh"

typedef struct funcdata     funcdata;
typedef struct pcodeop      pcodeop;
typedef struct varnode      varnode;
typedef struct flowblock    flowblock, blockbasic, blockgraph;
typedef struct dobc         dobc;
typedef struct jmptable     jmptable;
typedef struct func_call_specs  func_call_specs;
typedef map<Address, vector<varnode *>> variable_stack;
typedef struct valuetype    valuetype;

class pcodeemit2 : public PcodeEmit {
public:
    funcdata *fd = NULL;
    FILE *fp = stdout;
    virtual void dump(const Address &address, OpCode opc, VarnodeData *outvar, VarnodeData *vars, int size);

    void set_fp(FILE *f) { fp = f;  }
};

enum ARMInstType {
    a_null,
    a_stmdb,
    a_sub,
    a_add,
};

struct VisitStat {
    SeqNum seqnum;
    int size;
    struct {
        unsigned condinst: 1;
    } flags;

    enum ARMInstType    inst_type;
};

enum height {
    a_top,
    /* 
    普通常量:
    mov r0, 1
    这样的就是普通的常量, 1是常量, 赋给r0以后，当前的r0也成了常量 */

    a_constant,
    /*
    相对常量

    0x0001. add esp, 4
    0x0002. ...
    0x0003. ...
    0x0004. sub esp, 16
    
    esp就是相对常量，我们在做一些分析的时候，会使用模拟的方式来计算esp，比如给esp一个初始值，这种方式在分析领域不能算错，
    因为比如像IDA或者Ghidra，都有对错误的容忍能力，最终的结果还是需要开发人员自己判断，但是在编译还原领域，不能有这种模糊语义
    的行为。

    假设壳的开发人员，在某个代码中采取了判断esp值的方式来实现了部分功能，比如
    if (esp > xxxxx)  {} else {}
    很可能其中一个 condition_block 就会被判断成 unreachable code。这个是给esp强行赋值带来的结果，但是不赋值可能很多计算进行不下去

    所以我们把 inst.0x0001 中的 esp设置为相对常量(rel_constant)，他的值为 esp.4, 0x0004中的esp 为  esp.16，在做常量传播时，有以下几个规则

    1. const op const = const，      (常量和常量可以互相操作，比如 加减乘除之类)
    2. sp.rel_const op const = sp.rel_const;   (非常危险，要判断这个操作不是在循环内)
    3. sp.rel_const op sp.rel_const = sp.rel_const      (这种情况起始非常少见，)
    4. sp.rel_const op rN.rel_const (top)   (不同地址的相对常量不能参与互相运算)
    */ 
    a_rel_constant,
    a_bottom,

    /* */
};

struct valuetype {
    enum height height;
    intb v;
    Address rel;
};

struct varnode {
    valuetype   type;

    struct {
        unsigned    mark : 1;
        unsigned    annotation : 1;
        unsigned    input : 1;          // 没有祖先
        unsigned    writtern : 1;       // 是def
        unsigned    insert : 1;
        unsigned    implied : 1;        // 是一个临时变量
        unsigned    exlicit : 1;        // 不是临时变量

        unsigned    readonly : 1;

        unsigned    covertdirty : 1;    // cover没跟新
    } flags = { 0 };

    int size = 0;
    int create_index = 0;
    Address loc;

    pcodeop     *def = NULL;
    uintb       nzm;

    list<pcodeop *>     uses;    // descend, Ghidra把这个取名为descend，搞的我头晕，改成use

    varnode(int s, const Address &m);
    ~varnode();

    const Address &get_addr(void) const { return (const Address &)loc; }
    bool            is_heritage_known(void) const { return (flags.insert | flags.annotation) || is_constant(); }
    bool            has_no_use(void) { return uses.empty();  }

    void            set_def(pcodeop *op);
    bool            is_constant(void) const { return type.height == a_constant;  }
    void            set_val(intb v) { type.height = a_constant;  type.v = v; }
    bool            is_rel_constant(void) { return type.height == a_rel_constant;  }
    void            set_rel_constant(Address &r, int v) { type.height = a_rel_constant; type.v = v;  type.rel = r;  }
    void            add_use(pcodeop *op);
    intb            get_val(void);
    Address         &get_rel(void) { return type.rel; }
};

struct varnode_cmp_loc_def {
    bool operator()(const varnode *a, const varnode *b) const;
};

struct varnode_cmp_def_loc {
    bool operator()(const varnode *a, const varnode *b) const;
};

typedef set<varnode *, varnode_cmp_loc_def> varnode_loc_set;
typedef set<varnode *, varnode_cmp_def_loc> varnode_def_set;

struct pcodeop {
    struct {
        unsigned startblock : 1;
        unsigned branch : 1;
        unsigned call : 1;
        unsigned returns: 1;
        unsigned nocollapse : 1;
        unsigned dead : 1;
        unsigned marker : 1;        // 特殊的站位符， (phi 符号 或者 间接引用 或 CPUI_COPY 对同一个变量的操作)，
        unsigned boolouput : 1;     // 布尔操作

        unsigned coderef : 1;
        unsigned startmark : 1;     // instruction的第一个pcode
        unsigned mark : 1;          // 临时性标记，被某些算法拿过来做临时性处理，处理完都要重新清空

        unsigned branch_call : 1;   // 一般的跳转都是在函数内进行的，但是有些壳的函数，会直接branch到另外一个函数里面去
        unsigned exit : 1;          // 这个指令起结束作用
    } flags;

    OpCode opcode;
    /* 一个指令对应于多个pcode，这个是用来表示inst的 */
    SeqNum start;               
    flowblock *parent;
    /* 我们认为程序在分析的时候，sp的值是可以静态分析的，他表示的并不是sp寄存器，而是系统当前堆栈的深度 */
    int     sp;

    varnode *output;
    vector<varnode *> inrefs;

    list<pcodeop *>::iterator basiciter;
    list<pcodeop *>::iterator insertiter;
    list<pcodeop *>::iterator codeiter;

    pcodeop(int s, const SeqNum &sq);
    ~pcodeop();

    void            set_opcode(OpCode op);
    varnode*        get_in(int slot) { return inrefs[slot];  }
    const Address&  get_addr() { return start.getAddr();  }
    int             num_inputs() { return inrefs.size();  }
    int             get_slot(const varnode *vn) { 
        int i, n; n = inrefs.size(); 
        for (i = 0; i < n; i++)
            if (inrefs[i] == vn) return i;
        return -1;
    }
    int             dump(char *buf);
    void            compute(void);
    /* FIXME:判断哪些指令是别名安全的 */
    bool            is_safe_inst();
};

typedef struct blockedge            blockedge;

#define a_tree_edge             0x1
#define a_forward_edge          0x2
#define a_cross_edge            0x4
#define a_back_edge             0x8
#define a_loop_edge             0x10

struct blockedge {
    int label;
    flowblock *point;
    int reverse_index;

    blockedge(flowblock *pt, int lab, int rev) { point = pt, label = lab; reverse_index = rev; }
    blockedge() {};
};


enum block_type{
    a_condition,
    a_if,
    a_whiledo,
    a_dowhile,
    a_switch,
};

/* 模拟stack行为，*/
struct mem_stack {

    int size;
    char *data;
    
    mem_stack();
    ~mem_stack();

    void    push(char *byte, int size);
    int     top(int size);
    int     pop(int size);
};

struct flowblock {
    enum block_type     type;

    struct {
        unsigned f_goto_goto : 1;
        unsigned f_break_goto : 1;
        unsigned f_continue_goto : 1;
        unsigned f_entry_point : 1;
        unsigned f_dead : 1;

        unsigned f_switch_case : 1;
        unsigned f_switch_default : 1;

        /* 在某些算法中，做临时性标记用 */
        unsigned f_mark : 1;

        unsigned f_return : 1;
    } flags = { 0 };

    RangeList cover;

    list<pcodeop*>      ops;

    flowblock *parent = NULL;
    flowblock *immed_dom = NULL;

    int index = 0;
    int visitcount = 0;
    int numdesc = 0;        // 在 spaning tree中的后代数量

    vector<blockedge>   in;
    vector<blockedge>   out;
    vector<flowblock *> blist;

    jmptable *jmptable = NULL;

    funcdata *fd;

    flowblock(funcdata *fd);
    ~flowblock();

    void        add_block(flowblock *b);
    blockbasic* new_block_basic(funcdata *f);
    flowblock*  get_out(int i) { return out[i].point;  }
    flowblock*  get_in(int i) { return in[i].point;  }
    flowblock*  get_block(int i) { return blist[i]; }
    int         get_out_rev_index(int i) { return out[i].reverse_index;  }

    void        set_start_block(flowblock *bl);
    void        set_initial_range(const Address &begin, const Address &end);
    void        add_edge(flowblock *begin, flowblock *end);
    void        add_inedge(flowblock *b, int lab);
    void        add_op(pcodeop *);
    void        insert(list<pcodeop *>::iterator iter, pcodeop *inst);

    int         sub_id();
    void        structure_loops(vector<flowblock *> &rootlist);
    void        find_spanning_tree(vector<flowblock *> &preorder, vector<flowblock *> &rootlist);
    void        calc_forward_dominator(const vector<flowblock *> &rootlist);
    void        clear(void);
    void        build_dom_tree(vector<vector<flowblock *>> &child);
    int         build_dom_depth(vector<int> &depth);
    int         get_size(void) { return blist.size();  }
    Address     get_start(void);

};

typedef struct priority_queue   priority_queue;

struct priority_queue {
    vector<vector<flowblock *>> queue;
    int curdepth;

    priority_queue(void) { curdepth = -2;  }
    void reset(int maxdepth);
    void insert(flowblock *b, int depth);
    flowblock *extract();
    bool empty(void) const { return (curdepth == -1);  }
};

typedef map<SeqNum, pcodeop *>  pcodeop_tree;
typedef struct op_edge      op_edge;
typedef struct jmptable     jmptable;

struct op_edge {
    pcodeop *from;
    pcodeop *to;

    op_edge(pcodeop *from, pcodeop *to);
    ~op_edge();
} ;

struct jmptable {
    pcodeop *op;
    Address opaddr;
    int defaultblock;
    int lastblock;
    int size;

    vector<Address>     addresstable;

    jmptable(pcodeop *op);
    ~jmptable();
};

struct funcdata {
    struct {
        unsigned op_generated : 1;
        unsigned blocks_generated : 1;
        unsigned blocks_unreachable : 1;    // 有block无法到达
        unsigned processing_started : 1;
        unsigned processing_complete : 1;
        unsigned no_code : 1;
        unsigned unimplemented_present : 1;
        unsigned baddata_present : 1;
    } flags = { 0 };

    pcodeop_tree     optree;
    AddrSpace   *uniq_space = NULL;

    struct {
        funcdata *next = NULL;
        funcdata *prev = NULL;
    } node;

    list<op_edge *>    edgelist;

    /* jmp table */
    vector<pcodeop *>   tablelist;
    vector<jmptable *>  jmpvec;

    list<pcodeop *>     deadlist;
    list<pcodeop *>     storelist;
    list<pcodeop *>     loadlist;
    list<pcodeop *>     useroplist;
    list<pcodeop *>     deadandgone;
    int op_uniqid = 0;

    map<Address,VisitStat> visited;
    dobc *d = NULL;

    /* vbank------------------------- */
    struct {
        long uniqbase = 0;
        int uniqid = 0;
        int create_index = 0;
        struct dynarray all = { 0 };
    } vbank;

    varnode_loc_set     loc_tree;
    varnode_def_set     def_tree;
    varnode             searchvn;
    /* vbank------------------------- */

    /* control-flow graph */
    blockgraph bblocks;

    list<op_edge *>       block_edge;

    int     intput;         // 这个函数有几个输入参数
    int     output;         // 有几个输出参数
    vector<func_call_specs *>   qlst;

    /* heritage start ................. */
    vector<vector<flowblock *>> domchild;
    vector<vector<flowblock *>> augment;
#define boundary_node       1
#define mark_node           2
#define merged_node          3
    vector<uint4>   phiflags;   
    vector<int>     domdepth;
    /* dominate frontier */
    vector<flowblock *>     merge;      // 哪些block包含phi节点
    priority_queue pq;

    int maxdepth = -1;

    LocationMap     disjoint;
    LocationMap     globaldisjoint;
    /* FIXME:我不是很理解这个字段的意思，所以我没用他，一直恒为0 */
    int pass = 0;

    /* heritage end  ============================================= */

    Address startaddr;

    Address baddr;
    Address eaddr;
    string fullpath;
    string name;
    int size = 0;

    /* 扫描到的最小和最大指令地址 */
    Address minaddr;
    Address maxaddr;
    int inst_count = 0;
    int inst_max = 1000000;

    /* 这个区域内的所有*/
    RangeList   safezone;

    vector<Address>     addrlist;
    pcodeemit2 emitter;

    struct {
        int     size;
        u1      *bottom;
        u1      *top;
    } memstack;

    funcdata(const char *name, const Address &a, int size, dobc *d);
    ~funcdata(void);

    const Address&  get_addr(void) { return startaddr;  }
    string&      get_name() { return name;  }
    void        set_range(Address &b, Address &e) { baddr = b; eaddr = e; }

    pcodeop*    newop(int inputs, const SeqNum &sq);
    pcodeop*    newop(int inputs, const Address &pc);
    pcodeop*    cloneop(pcodeop *op, const SeqNum &seq);
    void        op_destroy_raw(pcodeop *op);

    varnode*    new_varnode_out(int s, const Address &m, pcodeop *op);
    varnode*    new_varnode(int s, AddrSpace *base, uintb off);
    varnode*    new_varnode(int s, const Address &m);
    varnode*    new_coderef(const Address &m);
    varnode*    clone_varnode(const varnode *vn);
    void        destroy_varnode(varnode *vn);
    void        delete_varnode(varnode *vn);
    /* 设置输入参数 */
    varnode*    set_input_varnode(varnode *vn);

    varnode*    create_vn(int s, const Address &m);
    varnode*    create_def(int s, const Address &m, pcodeop *op);
    varnode*    create_def_unique(int s, pcodeop *op);

    void        op_set_opcode(pcodeop *op, OpCode opc);
    void        op_set_input(pcodeop *op, varnode *vn, int slot);
    pcodeop*    find_op(const Address &addr);
    pcodeop*    find_op(const SeqNum &num) const;
    void        del_op(pcodeop *op);
    void        del_varnode(varnode *vn);

    varnode_loc_set::const_iterator     begin_loc(const Address &addr);
    varnode_loc_set::const_iterator     end_loc(const Address &addr);
    varnode_loc_set::const_iterator     begin_loc(AddrSpace *spaceid);
    varnode_loc_set::const_iterator     end_loc(AddrSpace *spaceid);

    void        del_remaining_ops(list<pcodeop *>::const_iterator oiter);
    void        new_address(pcodeop *from, const Address &to);
    pcodeop*    find_rel_target(pcodeop *op, Address &res) const;
    pcodeop*    target(const Address &addr) const;
    pcodeop*    branch_target(pcodeop *op);
    pcodeop*    fallthru_op(pcodeop *op);

    bool        set_fallthru_bound(Address &bound);
    void        fallthru();
    pcodeop*    xref_control_flow(list<pcodeop *>::const_iterator oiter, bool &startbasic, bool &isfallthru);
    void        generate_ops(void);
    bool        process_instruction(const Address &curaddr, bool &startbasic);
    void        recover_jmptable(pcodeop *op, int indexsize);
    void        analysis_jmptable(pcodeop *op);
    jmptable*   find_jmptable(pcodeop *op);

    void        collect_edges();
    void        generate_blocks();
    void        split_basic();
    void        connect_basic();

    void        dump_inst();
    void        dump_inst_dot(const char *postfix);
    void        dump_pcode(const char *postfix);

    void        remove_from_codelist(pcodeop *op);
    void        op_insert_before(pcodeop *op, pcodeop *follow);
    void        op_insert_after(pcodeop *op, pcodeop *prev);
    void        op_insert(pcodeop *op, blockbasic *bl, list<pcodeop *>::iterator iter);
    void        op_insert_begin(pcodeop *op, blockbasic *bl);
    void        op_insert_end(pcodeop *op, blockbasic *bl);
    void        inline_flow(funcdata *inlinefd, pcodeop *fd);
    void        inline_clone(funcdata *inelinefd, const Address &retaddr);
    void        inline_ezclone(funcdata *fd, const Address &calladdr);
    bool        check_ezmodel(void);
    void        structure_reset();

    void        mark_dead(pcodeop *op);
    void        mark_alive(pcodeop *op);
    void        fix_jmptable();
    char*       block_color(flowblock *b);
    void        build_dom_tree();
    void        start_processing(void);
    void        follow_flow(void);
    void        add_callspec(pcodeop *p, funcdata *fd);
    void        clear_blocks();
    int         inst_size(const Address &addr);
    void        build_adt(void);
    void        calc_phi_placement(const vector<varnode *> &write);
    void        visit_incr(flowblock *qnode, flowblock *vnode);
    void        place_multiequal(void);
    void        rename();
    void        rename_recurse(blockbasic *bl, variable_stack &varstack);
    int         collect(Address addr, int size, vector<varnode *> &read,
        vector<varnode *> &write, vector<varnode *> &input);
    void        heritage(void);
    void        constant_propagation();
    bool        is_ram(varnode *v);
    bool        is_sp_rel_constant(varnode *v);
    void        set_safezone(intb addr, int size);
    bool        in_safezone(intb addr, int size);

    intb        get_stack_value(intb offset, int size);
    void        set_stack_value(intb offset, int size, intb val);
};


struct func_call_specs {
    pcodeop *op;
    funcdata *fd;

    func_call_specs(pcodeop *o, funcdata *f);
    ~func_call_specs();

    const string &get_name(void) { return fd->name;  }
};

struct dobc {
    ElfLoadImage *loader;
    string slafilename;

    string fullpath;
    string filename;

    ContextDatabase *context = NULL;
    Translate *trans = NULL;
    TypeFactory *types;

    struct {
        int counts = 0;
        funcdata *list = NULL;
    } funcs;

    int max_basetype_size;
    int min_funcsymbol_size;
    int max_instructions;

    Address     sp_addr;
    Address     lr_addr;

    dobc(const char *slafilename, const char *filename);
    ~dobc();

    void init();

    /* 在一个函数内inline另外一个函数 */
    int inline_func(LoadImageFunc &func1, LoadImageFunc &func2);
    int loop_unrolling(LoadImageFunc &func1, Address &pos);
    /* 设置安全区，安全区内的代码是可以做别名分析的 */

    void analysis();
    void run();
    void dump_function(char *name);
    funcdata* find_func(const Address &addr);
    funcdata* find_func(const char *name);
    AddrSpace *get_code_space() { return trans->getDefaultCodeSpace();  }

    void plugin_dvmp360();
    void plugin_dvmp();

    void gen_sh(void);
};

// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_MINISCRIPT_H
#define BITCOIN_SCRIPT_MINISCRIPT_H

#include <algorithm>
#include <numeric>
#include <memory>
#include <string>
#include <vector>

#include <stdlib.h>
#include <assert.h>

#include <policy/policy.h>
#include <script/script.h>
#include <span.h>
#include <util/spanparsing.h>
#include <util/strencodings.h>
#include <util/vector.h>

namespace miniscript {

class compile_error : public std::runtime_error { public: explicit compile_error(const std::string& str) : std::runtime_error(str) {} };

/** This type encapsulates the miniscript type system properties.
 *
 * Every miniscript expression is one of 4 basic types, and additionally has
 * a number of boolean type properties.
 *
 * The basic types are:
 * - "B" Base:
 *   - Takes its inputs from the top of the stack.
 *   - When satisfied, pushes a nonzero value of up to 4 bytes onto the stack.
 *   - When dissatisfied, pushes a 0 onto the stack.
 *   - This is used for most expressions, and required for the top level one.
 *   - For example: older(n) = <n> OP_CHECKSEQUENCEVERIFY.
 * - "V" Verify:
 *   - Takes its inputs from the top of the stack.
 *   - When satisfactied, pushes nothing.
 *   - Cannot be dissatisfied.
 *   - This is obtained by adding an OP_VERIFY to a B, modifying the last opcode
 *     of a B to its -VERIFY version (only for OP_CHECKSIG, OP_CHECKSIGVERIFY
 *     and OP_EQUAL), or using IFs where both branches are also Vs.
 *   - For example vc:pk(key) = <key> OP_CHECKSIGVERIFY
 * - "K" Key:
 *   - Takes its inputs from the top of the stack.
 *   - Becomes a B when followed by OP_CHECKSIG.
 *   - Always pushes a public key onto the stack, for which a signature is to be
 *     provided to satisfy the expression.
 *   - For example pk_h(key) = OP_DUP OP_HASH160 <Hash160(key)> OP_EQUALVERIFY
 * - "W" Wrapped:
 *   - Takes its input from one below the top of the stack.
 *   - When satisfied, pushes a nonzero value (like B) on top of the stack, or one below.
 *   - When dissatisfied, pushes 0 op top of the stack or one below.
 *   - Is always "OP_SWAP [B]" or "OP_TOALTSTACK [B] OP_FROMALTSTACK".
 *   - For example sc:pk(key) = OP_SWAP <key> OP_CHECKSIG
 *
 * There a type properties that help reasoning about correctness:
 * - "z" Zero-arg:
 *   - Is known to always consume exactly 0 stack elements.
 *   - For example after(n) = <n> OP_CHECKLOCKTIMEVERIFY
 * - "o" One-arg:
 *   - Is known to always consume exactly 1 stack element.
 *   - Conflicts with property 'z'
 *   - For example sha256(hash) = OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 <hash> OP_EQUAL
 * - "n" Nonzero:
 *   - For every way this expression can be satisfied, a satisfaction exists that never needs
 *     a zero top stack element.
 *   - Conflicts with property 'z' and with type 'W'.
 * - "d" Dissatisfiable:
 *   - There is an easy way to construct a dissatisfaction for this expression.
 *   - Conflicts with type 'V'.
 * - "u" Unit:
 *   - In case of satisfaction, an exact 1 is put on the stack (rather than just nonzero).
 *   - Conflicts with type 'V'.
 *
 * Additional type properties help reasoning about nonmalleability:
 * - "e" Expression:
 *   - This implies property 'd', but the dissatisfaction is nonmalleable.
 *   - This generally requires 'e' for all subexpressions which are invoked for that
 *     dissatifsaction, and property 'f' for the unexecuted subexpressions in that case.
 *   - Conflicts with type 'V'.
 * - "f" Forced:
 *   - Dissatisfactions (if any) for this expression always involve at least one signature.
 *   - Is always true for type 'V'.
 * - "s" Safe:
 *   - Satisfactions for this expression always involve at least one signature.
 * - "m" Nonmalleable:
 *   - For every way this expression can be satisfied (which may be none),
 *     a nonmalleable satisfaction exists.
 *   - This generally requires 'm' for all subexpressions, and 'e' for all subexpressions
 *     which are dissatisfied when satisfying the parent.
 *
 * One final type property is an implementation detail:
 * - "x" Expensive verify:
 *   - Expressions with this property have a script whose last opcode is not EQUAL, CHECKSIG, or CHECKMULTISIG.
 *   - Not having this property means that it can be converted to a V at no cost (by switching to the
 *     -VERIFY version of the last opcode).
 *
 * For each of these properties the subset rule holds: an expression with properties X, Y, and Z, is also
 * valid in places where an X, a Y, a Z, an XY, ... is expected.
*/
class Type {
    //! Internal bitmap of properties (see ""_mst operator for details).
    uint16_t m_flags;

    //! Internal constructed used by the ""_mst operator.
    explicit constexpr Type(uint16_t flags) : m_flags(flags) {}

public:
    //! The only way to publicly construct a Type is using this literal operator.
    friend constexpr Type operator"" _mst(const char* c, size_t l);

    //! Compute the type with the union of properties.
    constexpr Type operator|(Type x) const { return Type(m_flags | x.m_flags); }

    //! Compute the type with the intersection of properties.
    constexpr Type operator&(Type x) const { return Type(m_flags & x.m_flags); }

    //! Check whether the left hand's properties are superset of the right's (= left is a subtype of right).
    constexpr bool operator<<(Type x) const { return (x.m_flags & ~m_flags) == 0; }

    //! Comparison operator to enable use in sets/maps (total ordering incompatible with <<).
    constexpr bool operator<(Type x) const { return m_flags < x.m_flags; }

    //! Equality operator.
    constexpr bool operator==(Type x) const { return m_flags == x.m_flags; }

    //! The empty type if x is false, itself otherwise.
    constexpr Type If(bool x) const { return Type(x ? m_flags : 0); }
};

//! Literal operator to construct Type objects.
inline constexpr Type operator"" _mst(const char* c, size_t l) {
    return l == 0 ? Type(0) : operator"" _mst(c + 1, l - 1) | Type(
        *c == 'B' ? 1 << 0 : // Base type
        *c == 'V' ? 1 << 1 : // Verify type
        *c == 'K' ? 1 << 2 : // Key type
        *c == 'W' ? 1 << 3 : // Wrapped type
        *c == 'z' ? 1 << 4 : // Zero-arg property
        *c == 'o' ? 1 << 5 : // One-arg property
        *c == 'n' ? 1 << 6 : // Nonzero arg property
        *c == 'd' ? 1 << 7 : // Dissatisfiable property
        *c == 'u' ? 1 << 8 : // Unit property
        *c == 'e' ? 1 << 9 : // Expression property
        *c == 'f' ? 1 << 10 : // Forced property
        *c == 's' ? 1 << 11 : // Safe property
        *c == 'm' ? 1 << 12 : // Nonmalleable property
        *c == 'x' ? 1 << 13 : // Expensive verify
        (throw std::logic_error("Unknown character in _mst literal"), 0)
    );
}

template<typename Key> struct Node;
template<typename Key> using NodeRef = std::shared_ptr<const Node<Key>>;

//! Construct a miniscript node as a shared_ptr.
template<typename Key, typename... Args>
NodeRef<Key> MakeNodeRef(Args&&... args) { return std::make_shared<const Node<Key>>(std::forward<Args>(args)...); }

//! The different node types in miniscript.
enum class NodeType {
    JUST_0,    //!< OP_0
    JUST_1,    //!< OP_1
    PK,        //!< [key]
    PK_H,      //!< OP_DUP OP_HASH160 [keyhash] OP_EQUALVERIFY
    OLDER,     //!< [n] OP_CHECKSEQUENCEVERIFY
    AFTER,     //!< [n] OP_CHECKLOCKTIMEVERIFY
    SHA256,    //!< OP_SIZE 32 OP_EQUALVERIFY OP_SHA256 [hash] OP_EQUAL
    HASH256,   //!< OP_SIZE 32 OP_EQUALVERIFY OP_HASH256 [hash] OP_EQUAL
    RIPEMD160, //!< OP_SIZE 32 OP_EQUALVERIFY OP_RIPEMD160 [hash] OP_EQUAL
    HASH160,   //!< OP_SIZE 32 OP_EQUALVERIFY OP_HASH160 [hash] OP_EQUAL
    WRAP_A,    //!< OP_TOALTSTACK [X] OP_FROMALTSTACK
    WRAP_S,    //!< OP_SWAP [X]
    WRAP_C,    //!< [X] OP_CHECKSIG
    WRAP_D,    //!< OP_DUP OP_IF [X] OP_ENDIF
    WRAP_V,    //!< [X] OP_VERIFY (or -VERIFY version of last opcode in X)
    WRAP_J,    //!< OP_SIZE OP_0NOTEQUAL OP_IF [X] OP_ENDIF
    WRAP_N,    //!< [X] OP_0NOTEQUAL
    AND_V,     //!< [X] [Y]
    AND_B,     //!< [X] [Y] OP_BOOLAND
    OR_B,      //!< [X] [Y] OP_BOOLOR
    OR_C,      //!< [X] OP_NOTIF [Y] OP_ENDIF
    OR_D,      //!< [X] OP_IFDUP OP_NOTIF [Y] OP_ENDIF
    OR_I,      //!< OP_IF [X] OP_ELSE [Y] OP_ENDIF
    ANDOR,     //!< [X] OP_NOTIF [Z] OP_ELSE [Y] OP_ENDIF
    THRESH,    //!< [X1] ([Xn] OP_ADD)* [k] OP_EQUAL
    THRESH_M,  //!< [k] [key_n]* [n] OP_CHECKMULTISIG
    // AND_N(X,Y) is represented as ANDOR(X,Y,0)
    // WRAP_T(X) is represented as AND_V(X,1)
    // WRAP_L(X) is represented as OR_I(0,X)
    // WRAP_U(X) is represented as OR_I(X,0)
    SIG,       //!< signature data
};

enum class Availability {
    NO,
    YES,
    MAYBE,
};

enum class EmitType : uint8_t {
    Default,
    Func,
    Key,
    Value,
    Modifier,
};

struct Entity {};

typedef std::map<size_t, const Entity*> Opmap;

struct StringEmitter {
    std::string m_str;
    virtual ~StringEmitter() {}
    virtual void emit(const Entity* ent, const std::string& value, bool own_line = false, bool ends_line = false, int indents = 0, bool strip_end = false, EmitType type = EmitType::Default) {
        m_str += value;
    }
    inline void emit(const Entity* ent, const std::string& value, EmitType type) {
        emit(ent, value, false, false, 0, false, type);
    }
    inline void emit_func_start(const Entity* ent, const std::string& prefix, const std::string& func, bool own_line = false, bool ends_line = false, int indents = 0, bool strip_end = false) {
        emit(ent, prefix, own_line, false, 0, strip_end);
        emit(ent, func, EmitType::Func);
        emit(ent, "(", false, ends_line, indents);
    }
    inline void emit_func(const Entity* ent, const std::string& prefix, const std::string& func, const std::string& content, EmitType content_type = EmitType::Default, bool own_line = false, bool ends_line = false, int indents = 0, bool strip_end = false) {
        emit_func_start(ent, prefix, func, own_line, false, 0, strip_end);
        emit(ent, content, content_type);
        emit(ent, ")", false, ends_line, indents);
    }
    virtual void set_avail(Availability avail) {}
};

extern std::shared_ptr<StringEmitter> DefaultStringEmitter;

/**
 * Show hash value for hashes, and string value for non-hashes.
 */
inline std::string HashValue(const std::vector<uint8_t>& hash, size_t hash_len) {
    if (hash.size() == hash_len) return HexStr(hash);
    return std::string((char*)&hash[0], (char*)&hash[hash.size()]);
}

namespace internal {

//! Helper function for Node::CalcType.
Type ComputeType(NodeType nodetype, Type x, Type y, Type z, const std::vector<Type>& sub_types, uint32_t k, size_t data_size, size_t n_subs, size_t n_keys);

//! Helper function for Node::CalcScriptLen.
size_t ComputeScriptLen(NodeType nodetype, Type sub0typ, size_t subsize, uint32_t k, size_t n_subs, size_t n_keys);

//! A helper sanitizer/checker for the output of CalcType.
Type SanitizeType(Type x);

//! An object representing a sequence of witness stack elements.
struct InputStack {
    /** Whether this stack is valid for its intended purpose (satisfaction or dissatisfaction of a Node).
     *  The MAYBE value is used for size estimation, when keys/preimages may actually be unavailable,
     *  but may be available at signing time. This makes the InputStack structure and signing logic,
     *  filled with dummy signatures/preimages usable for witness size estimation.
     */
    Availability available = Availability::YES;
    //! Whether this stack contains a digital signature.
    bool has_sig = false;
    //! Whether this stack is malleable (can be turned into an equally valid other stack by a third party).
    bool malleable = false;
    //! Whether this stack is non-canonical (using a construction known to be unnecessary for satisfaction).
    bool non_canon = false;
    //! Serialized witness size.
    size_t size = 0;
    //! Data elements.
    std::vector<std::vector<unsigned char>> stack;
    //! Construct an empty stack (valid).
    InputStack() {}
    //! Construct a valid single-element stack (with an element up to 75 bytes).
    InputStack(std::vector<unsigned char> in) : size(in.size() + 1), stack(Vector(std::move(in))) {}
    //! Change availability
    InputStack& Available(Availability avail);
    //! Mark this input stack as having a signature.
    InputStack& WithSig();
    //! Mark this input stack as non-canonical (known to not be necessary in non-malleable satisfactions).
    InputStack& NonCanon();
    //! Mark this input stack as malleable.
    InputStack& Malleable(bool x = true);
    //! Concatenate two input stacks.
    friend InputStack operator+(InputStack a, InputStack b);
    //! Choose between two potential input stacks.
    friend InputStack Choose(InputStack a, InputStack b, bool nonmalleable);
};

//! A pair of a satisfaction and a dissatisfaction InputStack.
struct InputResult {
    InputStack nsat, sat;
    InputResult(InputStack in_nsat, InputStack in_sat) : nsat(std::move(in_nsat)), sat(std::move(in_sat)) {}
};

//! Class whose objects represent the maximum of a list of integers.
template<typename I>
struct MaxInt {
    const bool valid;
    const I value;

    MaxInt() : valid(false), value(0) {}
    MaxInt(I val) : valid(true), value(val) {}

    friend MaxInt<I> operator+(const MaxInt<I>& a, const MaxInt<I>& b) {
        if (!a.valid || !b.valid) return {};
        return a.value + b.value;
    }

    friend MaxInt<I> Choose(const MaxInt<I>& a, const MaxInt<I>& b) {
        if (!a.valid) return b;
        if (!b.valid) return a;
        return std::max(a.value, b.value);
    }
};

struct Ops {
    //! Non-push opcodes.
    uint32_t stat;
    //! Number of keys in possibly executed OP_CHECKMULTISIG(VERIFY)s to satisfy.
    MaxInt<uint32_t> sat;
    //! Number of keys in possibly executed OP_CHECKMULTISIG(VERIFY)s to dissatisfy.
    MaxInt<uint32_t> dsat;

    Ops(uint32_t in_stat, MaxInt<uint32_t> in_sat, MaxInt<uint32_t> in_dsat) : stat(in_stat), sat(in_sat), dsat(in_dsat) {};
};

struct StackSize {
    //! Maximum stack size to satisfy;
    MaxInt<uint32_t> sat;
    //! Maximum stack size to dissatisfy;
    MaxInt<uint32_t> dsat;

    StackSize(MaxInt<uint32_t> in_sat, MaxInt<uint32_t> in_dsat) : sat(in_sat), dsat(in_dsat) {};
};

} // namespace internal

//! A node in a miniscript expression.
template<typename Key>
struct Node : public Entity {
    //! What node type this node is.
    const NodeType nodetype;
    //! The k parameter (time for OLDER/AFTER, threshold for THRESH(_M))
    const uint32_t k = 0;
    //! The keys used by this expression (only for PK/PK_H/THRESH_M)
    const std::vector<Key> keys;
    //! The data bytes in this expression (only for HASH160/HASH256/SHA256/RIPEMD10).
    const std::vector<unsigned char> data;
    //! Subexpressions (for WRAP_*/AND_*/OR_*/ANDOR/THRESH)
    const std::vector<NodeRef<Key>> subs;
    //! [approach] Whether this node is considered available at the time
    mutable Availability availability{Availability::MAYBE};

private:
    //! Cached ops counts.
    const internal::Ops ops;
    //! Cached stack size bounds.
    const internal::StackSize ss;
    //! Cached expression type (computed by CalcType and fed through SanitizeType).
    const Type typ;
    //! Cached script length (computed by CalcScriptLen).
    const size_t scriptlen;

    //! Compute the length of the script for this miniscript (including children).
    size_t CalcScriptLen() const {
        size_t subsize = 0;
        for (const auto& sub : subs) {
            subsize += sub->ScriptSize();
        }
        Type sub0type = subs.size() > 0 ? subs[0]->GetType() : ""_mst;
        return internal::ComputeScriptLen(nodetype, sub0type, subsize, k, subs.size(), keys.size());
    }

    //! Compute the type for this miniscript.
    Type CalcType() const {
        using namespace internal;

        // THRESH has a variable number of subexpression
        std::vector<Type> sub_types;
        if (nodetype == NodeType::THRESH) {
            for (const auto& sub : subs) sub_types.push_back(sub->GetType());
        }
        // All other nodes than THRESH can be computed just from the types of the 0-3 subexpexpressions.
        Type x = subs.size() > 0 ? subs[0]->GetType() : ""_mst;
        Type y = subs.size() > 1 ? subs[1]->GetType() : ""_mst;
        Type z = subs.size() > 2 ? subs[2]->GetType() : ""_mst;

        return SanitizeType(ComputeType(nodetype, x, y, z, sub_types, k, data.size(), subs.size(), keys.size()));
    }

    inline CScript MakeScriptAnnotated(const CScript& s, size_t& op, Opmap& nodeopmap) const {
        CScript::const_iterator it = s.begin();
        opcodetype opcode;
        std::vector<uint8_t> vchPushValue;
        while (s.GetOp(it, opcode, vchPushValue)) {
            if (nodeopmap.count(op) == 0) nodeopmap[op++] = this;
        }
        return s;
    }
    #define MSA(script) MakeScriptAnnotated(CScript() << script, op, nodeopmap)
    #define CMS(node) node->MakeScript(ctx, op, nodeopmap)
    #define CMSV(node, verify) node->MakeScript(ctx, op, nodeopmap, verify)

    //! Internal code for ToScript.
    template<typename Ctx>
    CScript MakeScript(const Ctx& ctx, size_t& op, Opmap& nodeopmap, bool verify = false) const {
        std::vector<unsigned char> bytes;
        switch (nodetype) {
            case NodeType::SIG: return MSA(data);// + CMS(subs[0]);
            case NodeType::PK: return MSA(ctx.ToPKBytes(keys[0]));
            case NodeType::PK_H: return MSA(OP_DUP << OP_HASH160 << ctx.ToPKHBytes(keys[0]) << OP_EQUALVERIFY);
            case NodeType::OLDER: return MSA(k << OP_CHECKSEQUENCEVERIFY);
            case NodeType::AFTER: return MSA(k << OP_CHECKLOCKTIMEVERIFY);
            case NodeType::SHA256: return MSA(OP_SIZE << 32 << OP_EQUALVERIFY << OP_SHA256 << data << (verify ? OP_EQUALVERIFY : OP_EQUAL));
            case NodeType::RIPEMD160: return MSA(OP_SIZE << 32 << OP_EQUALVERIFY << OP_RIPEMD160 << data << (verify ? OP_EQUALVERIFY : OP_EQUAL));
            case NodeType::HASH256: return MSA(OP_SIZE << 32 << OP_EQUALVERIFY << OP_HASH256 << data << (verify ? OP_EQUALVERIFY : OP_EQUAL));
            case NodeType::HASH160: return MSA(OP_SIZE << 32 << OP_EQUALVERIFY << OP_HASH160 << data << (verify ? OP_EQUALVERIFY : OP_EQUAL));
            case NodeType::WRAP_A: return MSA(OP_TOALTSTACK) + CMS(subs[0]) + MSA(OP_FROMALTSTACK);
            case NodeType::WRAP_S: return MSA(OP_SWAP) + CMSV(subs[0], verify);
            case NodeType::WRAP_C: return CMS(subs[0]) + MSA((verify ? OP_CHECKSIGVERIFY : OP_CHECKSIG));
            case NodeType::WRAP_D: return MSA(OP_DUP << OP_IF) + CMS(subs[0]) + MSA(OP_ENDIF);
            case NodeType::WRAP_V: return CMSV(subs[0], true) + (subs[0]->GetType() << "x"_mst ? MSA(OP_VERIFY) : CScript());
            case NodeType::WRAP_J: return MSA(OP_SIZE << OP_0NOTEQUAL << OP_IF) + CMS(subs[0]) + MSA(OP_ENDIF);
            case NodeType::WRAP_N: return CMS(subs[0]) + MSA(OP_0NOTEQUAL);
            case NodeType::JUST_1: return MSA(OP_1);
            case NodeType::JUST_0: return MSA(OP_0);
            case NodeType::AND_V: return CMS(subs[0]) + CMSV(subs[1], verify);
            case NodeType::AND_B: return CMS(subs[0]) + CMS(subs[1]) + MSA(OP_BOOLAND);
            case NodeType::OR_B: return CMS(subs[0]) + CMS(subs[1]) + MSA(OP_BOOLOR);
            case NodeType::OR_D: return CMS(subs[0]) + MSA(OP_IFDUP << OP_NOTIF) + CMS(subs[1]) + MSA(OP_ENDIF);
            case NodeType::OR_C: return CMS(subs[0]) + MSA(OP_NOTIF) + CMS(subs[1]) + MSA(OP_ENDIF);
            case NodeType::OR_I: return MSA(OP_IF) + CMS(subs[0]) + MSA(OP_ELSE) + CMS(subs[1]) + MSA(OP_ENDIF);
            case NodeType::ANDOR: return CMS(subs[0]) + MSA(OP_NOTIF) + CMS(subs[2]) + MSA(OP_ELSE) + CMS(subs[1]) + MSA(OP_ENDIF);
            case NodeType::THRESH_M: {
                CScript script = MSA(k);
                for (const auto& key : keys) {
                    script = script + MSA(ctx.ToPKBytes(key));
                }
                return script + MSA(keys.size() << (verify ? OP_CHECKMULTISIGVERIFY : OP_CHECKMULTISIG));
            }
            case NodeType::THRESH: {
                CScript script = CMS(subs[0]);
                for (size_t i = 1; i < subs.size(); ++i) {
                    script = (script + CMS(subs[i])) + MSA(OP_ADD);
                }
                return script + MSA(k << (verify ? OP_EQUALVERIFY : OP_EQUAL));
            }
        }
        throw compile_error("parse failure");
    }

    //! Internal code for ToString.
    template<typename Ctx>
    void MakeString(const Ctx& ctx, bool& success, bool wrapped, std::shared_ptr<StringEmitter> emitter) const {
        emitter->set_avail(availability);
        switch (nodetype) {
            case NodeType::WRAP_A: emitter->emit(this, "a", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_S: emitter->emit(this, "s", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_C: emitter->emit(this, "c", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_D: emitter->emit(this, "d", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_V: emitter->emit(this, "v", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_J: emitter->emit(this, "j", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::WRAP_N: emitter->emit(this, "n", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter);
            case NodeType::AND_V:
                // t:X is syntactic sugar for and_v(X,1).
                if (subs[1]->nodetype == NodeType::JUST_1) {
                    emitter->emit(this, "t", EmitType::Modifier);
                    return subs[0]->MakeString(ctx, success, true, emitter);
                }
                break;
            case NodeType::OR_I:
                if (subs[0]->nodetype == NodeType::JUST_0) { emitter->emit(this, "l", EmitType::Modifier); return subs[1]->MakeString(ctx, success, true, emitter); }
                if (subs[1]->nodetype == NodeType::JUST_0) { emitter->emit(this, "u", EmitType::Modifier); return subs[0]->MakeString(ctx, success, true, emitter); }
                break;
            default:
                break;
        }

        std::string ret = wrapped ? ":" : "";
        std::string sub_type;

        switch (nodetype) {
            case NodeType::PK: {
                std::string key_str;
                success = ctx.ToString(keys[0], key_str);
                return emitter->emit_func(this, std::move(ret), "pk", std::move(key_str), EmitType::Key, !wrapped);
            }
            case NodeType::PK_H: {
                std::string key_str;
                success = ctx.ToString(keys[0], key_str);
                return emitter->emit_func(this, std::move(ret), "pk_h", std::move(key_str), EmitType::Key, !wrapped);
            }
            case NodeType::AFTER: return emitter->emit_func(this, std::move(ret), "after", std::to_string(k), EmitType::Value, !wrapped);
            case NodeType::OLDER: return emitter->emit_func(this, std::move(ret), "older", std::to_string(k), EmitType::Value, !wrapped);
            case NodeType::SIG:
                emitter->emit_func(this, std::move(ret), "sig", HashValue(data, data.size()), EmitType::Value, !wrapped);
                // emitter->emit_func_start(this, std::move(ret), "sig", !wrapped, true, 4);
                // emitter->emit(this, HashValue(data, data.size()), EmitType::Value);
                // emitter->emit(this, ",", false, true, 0, true);
                // subs[0]->MakeString(ctx, success, false, emitter);
                // emitter->emit(this, "", false, true, -4);
                // emitter->emit(this, ")", false, true);
                return;
            case NodeType::HASH256: return emitter->emit_func(this, std::move(ret), "hash256", HashValue(data, 32), data.size() == 32 ? EmitType::Value : EmitType::Key, !wrapped);
            case NodeType::HASH160: return emitter->emit_func(this, std::move(ret), "hash160", HashValue(data, 20), data.size() == 20 ? EmitType::Value : EmitType::Key, !wrapped);
            case NodeType::SHA256: return emitter->emit_func(this, std::move(ret), "sha256", HashValue(data, 32), data.size() == 32 ? EmitType::Value : EmitType::Key, !wrapped);
            case NodeType::RIPEMD160: return emitter->emit_func(this, std::move(ret), "ripemd160", HashValue(data, 20), data.size() == 20 ? EmitType::Value : EmitType::Key, !wrapped);
            case NodeType::JUST_1: return emitter->emit(this, std::move(ret) + "1", EmitType::Value);
            case NodeType::JUST_0: return emitter->emit(this, std::move(ret) + "0", EmitType::Value);
            case NodeType::AND_V:
            case NodeType::AND_B:
                sub_type = nodetype == NodeType::AND_V ? "v" : "b";
                emitter->emit_func_start(this, std::move(ret), "and_" + sub_type, !wrapped, true, 4);
                subs[0]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, ",", false, true, 0, true);
                subs[1]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, "", false, true, -4);
                emitter->set_avail(availability);
                emitter->emit(this, ")", false, true);
                return;
            case NodeType::OR_B:
            case NodeType::OR_D:
            case NodeType::OR_C:
            case NodeType::OR_I:
                sub_type = "b";
                if (nodetype == NodeType::OR_D) sub_type = "d";
                if (nodetype == NodeType::OR_C) sub_type = "c";
                if (nodetype == NodeType::OR_I) sub_type = "i";
                emitter->emit_func_start(this, std::move(ret), "or_" + sub_type, !wrapped, true, 4);
                subs[0]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, ",", false, true, 0, true);
                subs[1]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, "", false, true, -4);
                emitter->set_avail(availability);
                emitter->emit(this, ")", false, true);
                return;
            case NodeType::ANDOR:
                // and_n(X,Y) is syntactic sugar for andor(X,Y,0).
                if (subs[2]->nodetype == NodeType::JUST_0) {
                    emitter->emit_func_start(this, std::move(ret), "and_n", !wrapped, true, 4);
                    subs[0]->MakeString(ctx, success, false, emitter);
                    emitter->emit(this, ",", false, true, 0, true);
                    subs[1]->MakeString(ctx, success, false, emitter);
                    emitter->emit(this, "", false, true, -4);
                    emitter->set_avail(availability);
                    emitter->emit(this, ")", false, true);
                    return;
                }
                emitter->emit_func_start(this, std::move(ret), "andor", !wrapped, true, 4);
                subs[0]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, ",", false, true, 0, true);
                subs[1]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, ",", false, true, 0, true);
                subs[2]->MakeString(ctx, success, false, emitter);
                emitter->emit(this, "", false, true, -4);
                emitter->set_avail(availability);
                emitter->emit(this, ")", false, true);
                return;
            case NodeType::THRESH_M: {
                emitter->emit_func_start(this, std::move(ret), "thresh_m", !wrapped, true, 4);
                emitter->emit(this, std::to_string(k), EmitType::Value);
                for (const auto& key : keys) {
                    std::string key_str;
                    success &= ctx.ToString(key, key_str);
                    emitter->emit(this, ",", false, true, 0, true);
                    emitter->emit(this, std::move(key_str), EmitType::Key);
                }
                emitter->emit(this, "", false, true, -4);
                emitter->set_avail(availability);
                emitter->emit(this, ")", false, true);
                return;
            }
            case NodeType::THRESH: {
                emitter->emit_func_start(this, std::move(ret), "thresh", !wrapped, true, 4);
                emitter->emit(this, std::to_string(k), EmitType::Value);
                for (const auto& sub : subs) {
                    emitter->emit(this, ",", false, true, 0, true);
                    sub->MakeString(ctx, success, false, emitter);
                }
                emitter->emit(this, "", false, true, -4);
                emitter->set_avail(availability);
                emitter->emit(this, ")", false, true);
                return;
            }
            default:
                throw compile_error("invalid fall-through (all cases should be covered)");
        }
    }

    internal::Ops CalcOps() const {
        switch (nodetype) {
            case NodeType::PK: return {0, 0, 0};
            case NodeType::PK_H: return {3, 0, 0};
            case NodeType::OLDER: return {1, 0, {}};
            case NodeType::AFTER: return {1, 0, {}};
            case NodeType::SHA256: return {4, 0, {}};
            case NodeType::RIPEMD160: return {4, 0, {}};
            case NodeType::HASH256: return {4, 0, {}};
            case NodeType::HASH160: return {4, 0, {}};
            case NodeType::SIG: return {1,0, {}}; // {1 + subs[0]->ops.stat, subs[0]->ops.stat, {}};
            case NodeType::AND_V: return {subs[0]->ops.stat + subs[1]->ops.stat, subs[0]->ops.sat + subs[1]->ops.sat, {}};
            case NodeType::AND_B: return {1 + subs[0]->ops.stat + subs[1]->ops.stat, subs[0]->ops.sat + subs[1]->ops.sat, subs[0]->ops.dsat + subs[1]->ops.dsat};
            case NodeType::OR_B: return {1 + subs[0]->ops.stat + subs[1]->ops.stat, Choose(subs[0]->ops.sat + subs[1]->ops.dsat, subs[1]->ops.sat + subs[0]->ops.dsat), subs[0]->ops.dsat + subs[1]->ops.dsat};
            case NodeType::OR_D: return {3 + subs[0]->ops.stat + subs[1]->ops.stat, Choose(subs[0]->ops.sat, subs[1]->ops.sat + subs[0]->ops.dsat), subs[0]->ops.dsat + subs[1]->ops.dsat};
            case NodeType::OR_C: return {2 + subs[0]->ops.stat + subs[1]->ops.stat, Choose(subs[0]->ops.sat, subs[1]->ops.sat + subs[0]->ops.dsat), {}};
            case NodeType::OR_I: return {3 + subs[0]->ops.stat + subs[1]->ops.stat, Choose(subs[0]->ops.sat, subs[1]->ops.sat), Choose(subs[0]->ops.dsat, subs[1]->ops.dsat)};
            case NodeType::ANDOR: return {3 + subs[0]->ops.stat + subs[1]->ops.stat + subs[2]->ops.stat, Choose(subs[1]->ops.sat + subs[0]->ops.sat, subs[0]->ops.dsat + subs[2]->ops.sat), subs[0]->ops.dsat + subs[2]->ops.dsat};
            case NodeType::THRESH_M: return {1, (uint32_t)keys.size(), (uint32_t)keys.size()};
            case NodeType::WRAP_A: return {2 + subs[0]->ops.stat, subs[0]->ops.sat, subs[0]->ops.dsat};
            case NodeType::WRAP_S: return {1 + subs[0]->ops.stat, subs[0]->ops.sat, subs[0]->ops.dsat};
            case NodeType::WRAP_C: return {1 + subs[0]->ops.stat, subs[0]->ops.sat, subs[0]->ops.dsat};
            case NodeType::WRAP_D: return {3 + subs[0]->ops.stat, subs[0]->ops.sat, 0};
            case NodeType::WRAP_V: return {subs[0]->ops.stat + (subs[0]->GetType() << "x"_mst), subs[0]->ops.sat, {}};
            case NodeType::WRAP_J: return {4 + subs[0]->ops.stat, subs[0]->ops.sat, 0};
            case NodeType::WRAP_N: return {1 + subs[0]->ops.stat, subs[0]->ops.sat, subs[0]->ops.dsat};
            case NodeType::JUST_1: return {0, 0, {}};
            case NodeType::JUST_0: return {0, {}, 0};
            case NodeType::THRESH: {
                uint32_t stat = 0;
                auto sats = Vector(internal::MaxInt<uint32_t>(0));
                for (const auto& sub : subs) {
                    stat += sub->ops.stat + 1;
                    auto next_sats = Vector(sats[0] + sub->ops.dsat);
                    for (size_t j = 1; j < sats.size(); ++j) next_sats.push_back(Choose(sats[j] + sub->ops.dsat, sats[j - 1] + sub->ops.sat));
                    next_sats.push_back(sats[sats.size() - 1] + sub->ops.sat);
                    sats = std::move(next_sats);
                }
                return {stat, sats[k], sats[0]};
            }
        }
        assert(false);
        return {0, {}, {}};
    }

    internal::StackSize CalcStackSize() const {
        switch (nodetype) {
            case NodeType::PK: return {1, 1};
            case NodeType::PK_H: return {2, 2};
            case NodeType::OLDER: return {0, {}};
            case NodeType::AFTER: return {0, {}};
            case NodeType::SHA256: return {1, {}};
            case NodeType::RIPEMD160: return {1, {}};
            case NodeType::HASH256: return {1, {}};
            case NodeType::HASH160: return {1, {}};
            case NodeType::SIG: return {1, {}};
            case NodeType::ANDOR: return {Choose(subs[0]->ss.sat + subs[1]->ss.sat, subs[0]->ss.dsat + subs[2]->ss.sat), subs[0]->ss.dsat + subs[2]->ss.dsat};
            case NodeType::AND_V: return {subs[0]->ss.sat + subs[1]->ss.sat, {}};
            case NodeType::AND_B: return {subs[0]->ss.sat + subs[1]->ss.sat, subs[0]->ss.dsat + subs[1]->ss.dsat};
            case NodeType::OR_B: return {Choose(subs[0]->ss.dsat + subs[1]->ss.sat, subs[0]->ss.sat + subs[1]->ss.dsat), subs[0]->ss.dsat + subs[1]->ss.dsat};
            case NodeType::OR_C: return {Choose(subs[0]->ss.sat, subs[0]->ss.dsat + subs[1]->ss.sat), {}};
            case NodeType::OR_D: return {Choose(subs[0]->ss.sat, subs[0]->ss.dsat + subs[1]->ss.sat), subs[0]->ss.dsat + subs[1]->ss.dsat};
            case NodeType::OR_I: return {Choose(subs[0]->ss.sat + 1, subs[1]->ss.sat + 1), Choose(subs[0]->ss.dsat + 1, subs[1]->ss.dsat + 1)};
            case NodeType::THRESH_M: return {(uint32_t)keys.size() + 1, (uint32_t)keys.size() + 1};
            case NodeType::WRAP_A: return subs[0]->ss;
            case NodeType::WRAP_S: return subs[0]->ss;
            case NodeType::WRAP_C: return subs[0]->ss;
            case NodeType::WRAP_D: return {1 + subs[0]->ss.sat, 1};
            case NodeType::WRAP_V: return {subs[0]->ss.sat, {}};
            case NodeType::WRAP_J: return {subs[0]->ss.sat, 1};
            case NodeType::WRAP_N: return subs[0]->ss;
            case NodeType::JUST_1: return {0, {}};
            case NodeType::JUST_0: return {{}, 0};
            case NodeType::THRESH: {
                auto sats = Vector(internal::MaxInt<uint32_t>(0));
                for (const auto& sub : subs) {
                    auto next_sats = Vector(sats[0] + sub->ss.dsat);
                    for (size_t j = 1; j < sats.size(); ++j) next_sats.push_back(Choose(sats[j] + sub->ss.dsat, sats[j - 1] + sub->ss.sat));
                    next_sats.push_back(sats[sats.size() - 1] + sub->ss.sat);
                    sats = std::move(next_sats);
                }
                return {sats[k], sats[0]};
            }
        }
        assert(false);
        return {{}, {}};
    }

    template<typename Ctx>
    internal::InputResult ProduceInput(const Ctx& ctx, bool nonmal) const {
        auto ret = ProduceInputHelper(ctx, nonmal);
        // Do a consistency check between the satisfaction code and the type checker
        // (the actual satisfaction code in ProduceInputHelper does not use GetType)
        if (GetType() << "z"_mst && ret.nsat.available != Availability::NO) assert(ret.nsat.stack.size() == 0);
        if (GetType() << "z"_mst && ret.sat.available != Availability::NO) assert(ret.sat.stack.size() == 0);
        if (GetType() << "o"_mst && ret.nsat.available != Availability::NO) assert(ret.nsat.stack.size() == 1);
        if (GetType() << "o"_mst && ret.sat.available != Availability::NO) assert(ret.sat.stack.size() == 1);
        if (GetType() << "n"_mst && ret.sat.available != Availability::NO) assert(ret.sat.stack.back().size() != 0);
        if (GetType() << "d"_mst) assert(ret.nsat.available != Availability::NO);
        if (GetType() << "f"_mst && ret.nsat.available != Availability::NO) assert(ret.nsat.has_sig);
        if (GetType() << "s"_mst && ret.sat.available != Availability::NO) assert(ret.sat.has_sig);
        if (nonmal) {
            if (GetType() << "d"_mst) assert(!ret.nsat.has_sig);
            if (GetType() << "d"_mst && !ret.nsat.malleable) assert(!ret.nsat.non_canon);
            if (GetType() << "e"_mst) assert(!ret.nsat.malleable);
            if (GetType() << "m"_mst && ret.sat.available != Availability::NO) assert(!ret.sat.malleable);
            if (ret.sat.available != Availability::NO && !ret.sat.malleable) assert(!ret.sat.non_canon);
        }
        return ret;
    }

    template<typename Ctx>
    internal::InputResult ProduceInputHelper(const Ctx& ctx, bool nonmal) const {
        using namespace internal;

        const auto ZERO = InputStack(std::vector<unsigned char>());
        const auto ZERO32 = InputStack(std::vector<unsigned char>(32, 0)).Malleable();
        const auto ONE = InputStack(Vector((unsigned char)1));
        const auto EMPTY = InputStack();
        const auto MALLEABLE_EMPTY = InputStack().Malleable();
        const auto INVALID = InputStack().Available(Availability::NO);

        switch (nodetype) {
            case NodeType::PK: {
                std::vector<unsigned char> sig;
                Availability avail = ctx.Sign(keys[0], sig);
                return InputResult(ZERO, InputStack(std::move(sig)).WithSig().Available(avail));
            }
            case NodeType::PK_H: {
                std::vector<unsigned char> key = ctx.ToPKBytes(keys[0]), sig;
                Availability avail = ctx.Sign(keys[0], sig);
                return InputResult(ZERO + InputStack(key), (InputStack(std::move(sig)).WithSig() + InputStack(key)).Available(avail));
            }
            case NodeType::SIG: {
                return InputResult(ZERO, ZERO); // subs[0]->ProduceInputHelper(ctx, nonmal);
            }
            case NodeType::THRESH_M: {
                std::vector<InputStack> sats = Vector(ZERO);
                for (size_t i = 0; i < keys.size(); ++i) {
                    std::vector<unsigned char> sig;
                    Availability avail = ctx.Sign(keys[i], sig);
                    auto sat = InputStack(std::move(sig)).WithSig().Available(avail);
                    std::vector<InputStack> next_sats;
                    next_sats.push_back(sats[0]);
                    for (size_t j = 1; j < sats.size(); ++j) next_sats.push_back(Choose(sats[j], std::move(sats[j - 1]) + sat, nonmal));
                    next_sats.push_back(std::move(sats[sats.size() - 1]) + std::move(sat));
                    sats = std::move(next_sats);
                }
                InputStack nsat = ZERO;
                for (size_t i = 0; i < k; ++i) nsat = std::move(nsat) + ZERO;
                return InputResult(std::move(nsat), std::move(sats[k]));
            }
            case NodeType::THRESH: {
                std::vector<InputStack> sats = Vector(EMPTY);
                for (size_t i = 0; i < subs.size(); ++i) {
                    auto res = subs[subs.size() - i - 1]->ProduceInput(ctx, nonmal);
                    std::vector<InputStack> next_sats;
                    next_sats.push_back(sats[0] + res.nsat);
                    for (size_t j = 1; j < sats.size(); ++j) next_sats.push_back(Choose(sats[j] + res.nsat, std::move(sats[j - 1]) + res.sat, nonmal));
                    next_sats.push_back(std::move(sats[sats.size() - 1]) + std::move(res.sat));
                    sats = std::move(next_sats);
                }
                InputStack nsat = INVALID;
                for (size_t i = 0; i < sats.size(); ++i) {
                    if (i != k) nsat = Choose(std::move(nsat), std::move(sats[i]), nonmal);
                }
                return InputResult(std::move(nsat), std::move(sats[k]));
            }
            case NodeType::OLDER: {
                return InputResult(INVALID, ctx.CheckOlder(k) ? EMPTY : INVALID);
            }
            case NodeType::AFTER: {
                return InputResult(INVALID, ctx.CheckAfter(k) ? EMPTY : INVALID);
            }
            case NodeType::SHA256: {
                std::vector<unsigned char> preimage;
                Availability avail = ctx.SatSHA256(data, preimage);
                return InputResult(ZERO32, InputStack(std::move(preimage)).Available(avail));
            }
            case NodeType::RIPEMD160: {
                std::vector<unsigned char> preimage;
                Availability avail = ctx.SatRIPEMD160(data, preimage);
                return InputResult(ZERO32, InputStack(std::move(preimage)).Available(avail));
            }
            case NodeType::HASH256: {
                std::vector<unsigned char> preimage;
                Availability avail = ctx.SatHASH256(data, preimage);
                return InputResult(ZERO32, InputStack(std::move(preimage)).Available(avail));
            }
            case NodeType::HASH160: {
                std::vector<unsigned char> preimage;
                Availability avail = ctx.SatHASH160(data, preimage);
                return InputResult(ZERO32, InputStack(std::move(preimage)).Available(avail));
            }
            case NodeType::AND_V: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), y = subs[1]->ProduceInput(ctx, nonmal);
                return InputResult((y.nsat + x.sat).NonCanon(), y.sat + x.sat);
            }
            case NodeType::AND_B: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), y = subs[1]->ProduceInput(ctx, nonmal);
                return InputResult(Choose(Choose(y.nsat + x.nsat, (y.sat + x.nsat).NonCanon(), nonmal), (y.nsat + x.sat).NonCanon(), nonmal), y.sat + x.sat);
            }
            case NodeType::OR_B: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), z = subs[1]->ProduceInput(ctx, nonmal);
                return InputResult(z.nsat + x.nsat, Choose(Choose(z.nsat + x.sat, z.sat + x.nsat, nonmal), (z.sat + x.sat).NonCanon(), nonmal));
            }
            case NodeType::OR_C: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), z = subs[1]->ProduceInput(ctx, nonmal);
                return InputResult(INVALID, Choose(x.sat, z.sat + x.nsat, nonmal));
            }
            case NodeType::OR_D: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), z = subs[1]->ProduceInput(ctx, nonmal);
                auto nsat = z.nsat + x.nsat, sat_l = x.sat, sat_r = z.sat + x.nsat;
                return InputResult(z.nsat + x.nsat, Choose(x.sat, z.sat + x.nsat, nonmal));
            }
            case NodeType::OR_I: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), z = subs[1]->ProduceInput(ctx, nonmal);
                return InputResult(Choose(x.nsat + ONE, z.nsat + ZERO, nonmal), Choose(x.sat + ONE, z.sat + ZERO, nonmal));
            }
            case NodeType::ANDOR: {
                auto x = subs[0]->ProduceInput(ctx, nonmal), y = subs[1]->ProduceInput(ctx, nonmal), z = subs[2]->ProduceInput(ctx, nonmal);
                return InputResult(Choose((y.nsat + x.sat).NonCanon(), z.nsat + x.nsat, nonmal), Choose(y.sat + x.sat, z.sat + x.nsat, nonmal));
            }
            case NodeType::WRAP_A:
            case NodeType::WRAP_S:
            case NodeType::WRAP_C:
            case NodeType::WRAP_N:
                return subs[0]->ProduceInput(ctx, nonmal);
            case NodeType::WRAP_D: {
                auto x = subs[0]->ProduceInput(ctx, nonmal);
                return InputResult(ZERO, x.sat + ONE);
            }
            case NodeType::WRAP_J: {
                auto x = subs[0]->ProduceInput(ctx, nonmal);
                // If a dissatisfaction with a nonzero top stack element exists, an alternative dissatisfaction exists.
                // As the dissatisfaction logic currently doesn't keep track of this nonzeroness property, and thus even
                // if a dissatisfaction with a top zero element is found, we don't know whether another one with a
                // nonzero top stack element exists. Make the conservative assumption that whenever the subexpression is weakly
                // dissatisfiable, this alternative dissatisfaction exists and leads to malleability.
                return InputResult(InputStack(ZERO).Malleable(x.nsat.available != Availability::NO && !x.nsat.has_sig), x.sat);
            }
            case NodeType::WRAP_V: {
                auto x = subs[0]->ProduceInput(ctx, nonmal);
                return InputResult(INVALID, x.sat);
            }
            case NodeType::JUST_0: return InputResult(EMPTY, INVALID);
            case NodeType::JUST_1: return InputResult(INVALID, EMPTY);
        }
        throw compile_error("failed to generate input result");
    }

public:
    template<typename Ctx>
    Availability CalcAvail(const Ctx& ctx, const std::set<std::string>& inventory, const std::set<std::string>& missing, int confirmations) const {
        std::set<std::string> required;
        std::vector<NodeRef<Key>> dependencies;
        std::string key;
        bool guaranteed_maybe = false;
        int threshold = 1; // # of dependencies which must be satisfied for availability
        switch (nodetype) {
            case NodeType::THRESH_M: {
                threshold = k;
            case NodeType::PK:
            case NodeType::PK_H:
                for (const auto& v : keys) {
                    ctx.ToString(v, key);
                    required.insert(key);
                }
                break;
            case NodeType::OLDER:
                return availability = confirmations == -1 ? Availability::MAYBE : confirmations >= k ? Availability::YES : Availability::NO;
            // case NodeType::AFTER:
            #define preimage(s, len) \
                required.insert(std::string(#s "^-1(" + HashValue(data, len) + ")")); \
                break
            case NodeType::SHA256:    preimage(sha256, 32);
            case NodeType::RIPEMD160: preimage(ripemd160, 20);
            case NodeType::HASH256:   preimage(hash256, 32);
            case NodeType::HASH160:   preimage(hash160, 20);
            #undef preimage
            case NodeType::SIG:
                return availability = Availability::YES;
            case NodeType::WRAP_A:
            case NodeType::WRAP_S:
            case NodeType::WRAP_C:
            case NodeType::WRAP_D:
            case NodeType::WRAP_V:
            case NodeType::WRAP_J:
            case NodeType::WRAP_N:
                dependencies.push_back(subs[0]);
                break;
            case NodeType::ANDOR:
                // (0 & 1) | 2: we check 2 case and then use (0 & 1)
                {
                    auto a = subs[2]->CalcAvail(ctx, inventory, missing, confirmations);
                    if (a == Availability::YES) {
                        return availability = Availability::YES;
                    }
                    guaranteed_maybe = a == Availability::MAYBE;
                }
            case NodeType::AND_V:
            case NodeType::AND_B:
                threshold = 2;
                dependencies.push_back(subs[0]); dependencies.push_back(subs[1]);
                break;
            case NodeType::OR_B:
            case NodeType::OR_D:
            case NodeType::OR_C:
            case NodeType::OR_I:
                dependencies.push_back(subs[0]); dependencies.push_back(subs[1]);
                break;
            }
            case NodeType::THRESH: {
                threshold = k;
                dependencies.insert(dependencies.end(), subs.begin(), subs.end());
                break;
            }
            default: return availability = Availability::YES;
        }

        size_t possible = required.size() + dependencies.size();
        size_t satisfactions = 0;
        size_t dissatisfactions = 0;
        for (const auto& k : required) {
            satisfactions += inventory.count(k);
            dissatisfactions += missing.count(k);
        }

        for (auto& dep : dependencies) {
            Availability sub_avail = dep->CalcAvail(ctx, inventory, missing, confirmations);
            satisfactions += sub_avail == Availability::YES;
            dissatisfactions += sub_avail == Availability::NO;
        }

        if (satisfactions >= threshold) return availability = Availability::YES;
        if (!guaranteed_maybe && possible - dissatisfactions < threshold) return availability = Availability::NO;
        // inconclusive
        return availability = Availability::MAYBE;
    }

    //! Return the size of the script for this expression (faster than ToString().size()).
    size_t ScriptSize() const { return scriptlen; }

    //! Return the maximum number of ops needed to satisfy this script non-malleably.
    uint32_t GetOps() const { return ops.stat + ops.sat.value; }

    //! Check the ops limit of this script against the consensus limit.
    bool CheckOpsLimit() const { return GetOps() <= MAX_OPS_PER_SCRIPT; }

    //! Return the maximum number of stack elements needed to satisfy this script non-malleably.
    uint32_t GetStackSize() const { return ss.sat.value; }

    //! Check the maximum stack size for this script against the policy limit.
    bool CheckStackSize() const { return GetStackSize() <= MAX_STANDARD_P2WSH_STACK_ITEMS; }

    //! Return the expression type.
    Type GetType() const { return typ; }

    //! Check whether this node is valid at all.
    bool IsValid() const { return !(GetType() == ""_mst); }

    //! Check whether this node is valid as a script on its own.
    bool IsValidTopLevel() const { return GetType() << "B"_mst; }

    //! Check whether this script can always be satisfied in a non-malleable way.
    bool IsNonMalleable() const { return GetType() << "m"_mst; }

    //! Check whether this script always needs a signature.
    bool NeedsSignature() const { return GetType() << "s"_mst; }

    //! Do all sanity checks.
    bool IsSafeTopLevel() const { return GetType() << "Bms"_mst && CheckOpsLimit() && CheckStackSize(); }

    //! Construct the script for this miniscript (including subexpressions).
    template<typename Ctx>
    CScript ToScript(const Ctx& ctx, Opmap& nodeopmap) const { size_t op = 0; return MakeScript(ctx, op, nodeopmap); }

    //! Convert this miniscript to its textual descriptor notation.
    template<typename Ctx>
    bool ToString(const Ctx& ctx, std::string& out, bool wrapped = false, std::shared_ptr<StringEmitter> emitter = DefaultStringEmitter) const {
        bool ret = true;
        emitter->m_str = "";
        MakeString(ctx, ret, wrapped, emitter);
        if (ret) out = emitter->m_str;
        return ret;
    }

    template<typename Ctx>
    Availability Satisfy(const Ctx& ctx, std::vector<std::vector<unsigned char>>& stack, bool nonmalleable = true) const {
        auto ret = ProduceInput(ctx, nonmalleable);
        if (nonmalleable && (ret.sat.malleable || !ret.sat.has_sig)) return Availability::NO;
        stack = std::move(ret.sat.stack);
        return ret.sat.available;
    }

    //! Equality testing.
    bool operator==(const Node<Key>& arg) const
    {
        if (nodetype != arg.nodetype) return false;
        if (k != arg.k) return false;
        if (data != arg.data) return false;
        if (keys != arg.keys) return false;
        if (subs.size() != arg.subs.size()) return false;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (!(*subs[i] == *arg.subs[i])) return false;
        }
        assert(scriptlen == arg.scriptlen);
        assert(typ == arg.typ);
        return true;
    }

    // Constructors with various argument combinations.
    Node(NodeType nt, std::vector<NodeRef<Key>> sub, std::vector<unsigned char> arg, uint32_t val = 0) : nodetype(nt), k(val), data(std::move(arg)), subs(std::move(sub)), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
    Node(NodeType nt, std::vector<unsigned char> arg, uint32_t val = 0) : nodetype(nt), k(val), data(std::move(arg)), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
    Node(NodeType nt, std::vector<NodeRef<Key>> sub, std::vector<Key> key, uint32_t val = 0) : nodetype(nt), k(val), keys(std::move(key)), subs(std::move(sub)), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
    Node(NodeType nt, std::vector<Key> key, uint32_t val = 0) : nodetype(nt), k(val), keys(std::move(key)), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
    Node(NodeType nt, std::vector<NodeRef<Key>> sub, uint32_t val = 0) : nodetype(nt), k(val), subs(std::move(sub)), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
    Node(NodeType nt, uint32_t val = 0) : nodetype(nt), k(val), ops(CalcOps()), ss(CalcStackSize()), typ(CalcType()), scriptlen(CalcScriptLen()) {}
};

namespace internal {

// Parse(...) is recursive. Recursion depth is limited to MAX_PARSE_RECURSION to avoid
// running out of stack space at run-time. It is impossible to create a valid Miniscript
// with a nesting depth higher than 402 (any such script will trivially exceed the ops
// limit of 201). Those 402 consist of 201 v: wrappers and 201 other nodes. The Parse
// functions don't use recursion for wrappers, so the recursion limit can be 201.
static constexpr int MAX_PARSE_RECURSION = 201;

//! Parse a miniscript from its textual descriptor form.
template<typename Key, typename Ctx>
inline NodeRef<Key> Parse(Span<const char>& in, const Ctx& ctx, int recursion_depth) {
    static size_t counter = 0;
    ++counter;
    using namespace spanparsing;
    if (recursion_depth >= MAX_PARSE_RECURSION) {
        throw compile_error("max recursion depth exceeded");
    }
    auto expr = Expr(in);
    // Parse wrappers
    for (int i = 0; i < expr.size(); ++i) {
        if (expr[i] == ':') {
            auto in2 = expr.subspan(i + 1);
            auto sub = Parse<Key>(in2, ctx, recursion_depth + 1);
            if (!sub || in2.size()) {
                throw compile_error("failed to parse wrapper");
            }
            for (int j = i; j-- > 0; ) {
                if (expr[j] == 'a') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_A, Vector(std::move(sub)));
                } else if (expr[j] == 's') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_S, Vector(std::move(sub)));
                } else if (expr[j] == 'c') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_C, Vector(std::move(sub)));
                } else if (expr[j] == 'd') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_D, Vector(std::move(sub)));
                } else if (expr[j] == 'j') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_J, Vector(std::move(sub)));
                } else if (expr[j] == 'n') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_N, Vector(std::move(sub)));
                } else if (expr[j] == 'v') {
                    sub = MakeNodeRef<Key>(NodeType::WRAP_V, Vector(std::move(sub)));
                } else if (expr[j] == 't') {
                    sub = MakeNodeRef<Key>(NodeType::AND_V, Vector(std::move(sub), MakeNodeRef<Key>(NodeType::JUST_1)));
                } else if (expr[j] == 'u') {
                    sub = MakeNodeRef<Key>(NodeType::OR_I, Vector(std::move(sub), MakeNodeRef<Key>(NodeType::JUST_0)));
                } else if (expr[j] == 'l') {
                    sub = MakeNodeRef<Key>(NodeType::OR_I, Vector(MakeNodeRef<Key>(NodeType::JUST_0), std::move(sub)));
                } else {
                    throw compile_error("exhausted expression types (unknown type encountered)");
                }
            }
            return sub;
        }
        if (expr[i] < 'a' || expr[i] > 'z') break;
    }
    // Parse the other node types
    NodeType nodetype;
    if (expr == Span<const char>("0", 1)) {
        return MakeNodeRef<Key>(NodeType::JUST_0);
    } else if (expr == Span<const char>("1", 1)) {
        return MakeNodeRef<Key>(NodeType::JUST_1);
    } else if (Func("pk", expr)) {
        Key key;
        if (ctx.FromString(expr.begin(), expr.end(), key)) {
            return MakeNodeRef<Key>(NodeType::PK, Vector(std::move(key)));
        }
        throw compile_error("failed to retrieve direct pubkey from string via context");
    } else if (Func("pk_h", expr)) {
        Key key;
        if (ctx.FromString(expr.begin(), expr.end(), key)) {
            return MakeNodeRef<Key>(NodeType::PK_H, Vector(std::move(key)));
        }
        throw compile_error("failed to retrieve pubkey via pubkey hash from string via context");
    } else if (expr == MakeSpan("0")) {
        return MakeNodeRef<Key>(NodeType::JUST_0);
    } else if (expr == MakeSpan("1")) {
        return MakeNodeRef<Key>(NodeType::JUST_1);
    } else if (Func("sha256", expr)) {
        auto hash = ParseHex(std::string(expr.begin(), expr.end()));
        if (hash.size() != 32) throw compile_error("hash size invalid for sha256 (must be 32)");
        return MakeNodeRef<Key>(NodeType::SHA256, std::move(hash));
    } else if (Func("ripemd160", expr)) {
        auto hash = ParseHex(std::string(expr.begin(), expr.end()));
        if (hash.size() != 20) throw compile_error("hash size invalid for ripemd160 (must be 20)");
        return MakeNodeRef<Key>(NodeType::RIPEMD160, std::move(hash));
    } else if (Func("hash256", expr)) {
        auto hash = ParseHex(std::string(expr.begin(), expr.end()));
        if (hash.size() != 32) throw compile_error("hash size invalid for hash256 (must be 32)");
        return MakeNodeRef<Key>(NodeType::HASH256, std::move(hash));
    } else if (Func("hash160", expr)) {
        auto hash = ParseHex(std::string(expr.begin(), expr.end()));
        if (hash.size() != 20) throw compile_error("hash size invalid for hash160 (must be 20)");
        return MakeNodeRef<Key>(NodeType::HASH160, std::move(hash));
    } else if (Func("push", expr)) {
        size_t i;
        for (i = 0; expr[i] && expr[i] != ','; ++i);
        auto hash = ParseHex(std::string(expr.begin(), expr.begin() + i));
        // expr = expr.subspan(i);
        // auto rem = Parse<Key>(expr, ctx, recursion_depth + 1);
        // if (!rem) throw compile_error("signature suffix (pubkey + OP_CHECKSIG, usually) not found/failed to parse");
        return MakeNodeRef<Key>(NodeType::SIG/*, Vector(std::move(rem))*/, std::move(hash));
    } else if (Func("after", expr)) {
        int64_t num;
        if (!ParseInt64(std::string(expr.begin(), expr.end()), &num)) throw compile_error("unable to parse int64 value");
        if (num < 1 || num >= 0x80000000L) throw compile_error("number is out of range 0x01..0x80000000");
        return MakeNodeRef<Key>(NodeType::AFTER, num);
    } else if (Func("older", expr)) {
        int64_t num;
        if (!ParseInt64(std::string(expr.begin(), expr.end()), &num)) throw compile_error("unable to parse int64 value");
        if (num < 1 || num >= 0x80000000L) throw compile_error("number is out of range 0x01..0x80000000");
        return MakeNodeRef<Key>(NodeType::OLDER, num);
    } else if (Func("and_n", expr)) {
        auto left = Parse<Key>(expr, ctx, recursion_depth + 1);
        if (!left || !Const(",", expr)) throw compile_error("left side not found or ',' not encountered");
        auto right = Parse<Key>(expr, ctx, recursion_depth + 1);
        if (!right || expr.size()) throw compile_error("right side not found or leftover data in expression");
        return MakeNodeRef<Key>(NodeType::ANDOR, Vector(std::move(left), std::move(right), MakeNodeRef<Key>(NodeType::JUST_0)));
    } else if (Func("andor", expr)) {
        auto left = Parse<Key>(expr, ctx, recursion_depth + 1);
        if (!left || !Const(",", expr)) throw compile_error("left not found or ',' not encountered");
        auto mid = Parse<Key>(expr, ctx, recursion_depth + 1);
        if (!mid || !Const(",", expr)) throw compile_error("middle not found or ',' not encountered");
        auto right = Parse<Key>(expr, ctx, recursion_depth + 1);
        if (!right || expr.size()) throw compile_error("right not found or leftover data in expression");
        return MakeNodeRef<Key>(NodeType::ANDOR, Vector(std::move(left), std::move(mid), std::move(right)));
    } else if (Func("thresh_m", expr)) {
        auto arg = Expr(expr);
        int64_t count;
        if (!ParseInt64(std::string(arg.begin(), arg.end()), &count)) throw compile_error("failed to parse int64");
        std::vector<Key> keys;
        while (expr.size()) {
            if (!Const(",", expr)) throw compile_error("',' not encountered between expressions");
            auto keyarg = Expr(expr);
            Key key;
            if (!ctx.FromString(keyarg.begin(), keyarg.end(), key)) throw compile_error("failed to retrieve key from string via context");
            keys.push_back(std::move(key));
        }
        if (keys.size() < 1 || keys.size() > 20) throw compile_error("key count is out of range 1..20");
        if (count < 1 || count > (int64_t)keys.size()) throw compile_error("threshold count is out of range 1..(key size)");
        return MakeNodeRef<Key>(NodeType::THRESH_M, std::move(keys), count);
    } else if (Func("thresh", expr)) {
        auto arg = Expr(expr);
        int64_t count;
        if (!ParseInt64(std::string(arg.begin(), arg.end()), &count)) throw compile_error("failed to parse int64");
        std::vector<NodeRef<Key>> subs;
        while (expr.size()) {
            if (!Const(",", expr)) throw compile_error("',' not encountered between expressions");
            auto sub = Parse<Key>(expr, ctx, recursion_depth + 1);
            if (!sub) throw compile_error("failed to parse expression");
            subs.push_back(std::move(sub));
        }
        if (count <= 1 || count >= (int64_t)subs.size()) throw compile_error("threshold count is out of range 2..(key size)");
        return MakeNodeRef<Key>(NodeType::THRESH, std::move(subs), count);
    } else if (Func("and_v", expr)) {
        nodetype = NodeType::AND_V;
    } else if (Func("and_b", expr)) {
        nodetype = NodeType::AND_B;
    } else if (Func("or_c", expr)) {
        nodetype = NodeType::OR_C;
    } else if (Func("or_b", expr)) {
        nodetype = NodeType::OR_B;
    } else if (Func("or_d", expr)) {
        nodetype = NodeType::OR_D;
    } else if (Func("or_i", expr)) {
        nodetype = NodeType::OR_I;
    } else {
        throw compile_error("exhausted node types");
    }
    auto left = Parse<Key>(expr, ctx, recursion_depth + 1);
    if (!left || !Const(",", expr)) throw compile_error("failed to parse left expression or ',' not encountered");
    auto right = Parse<Key>(expr, ctx, recursion_depth + 1);
    if (!right || expr.size()) throw compile_error("failed to parse right expression or leftover data in expression");
    return MakeNodeRef<Key>(nodetype, Vector(std::move(left), std::move(right)));
}

/** Decode a script into opcode/push pairs.
 *
 * Construct a vector with one element per opcode in the script, in reverse order.
 * Each element is a pair consisting of the opcode, as well as the data pushed by
 * the opcode (including OP_n), if any. OP_CHECKSIGVERIFY, OP_CHECKMULTISIGVERIFY,
 * and OP_EQUALVERIFY are decomposed into OP_CHECKSIG, OP_CHECKMULTISIG, OP_EQUAL
 * respectively, plus OP_VERIFY.
 */
bool DecomposeScript(const CScript& script, std::vector<std::pair<opcodetype, std::vector<unsigned char>>>& out);

/** Determine whether the passed pair (created by DecomposeScript) is pushing a number. */
bool ParseScriptNumber(const std::pair<opcodetype, std::vector<unsigned char>>& in, int64_t& k);

template<typename Key, typename Ctx, typename I> inline NodeRef<Key> DecodeSingle(I& in, I last, const Ctx& ctx);
template<typename Key, typename Ctx, typename I> inline NodeRef<Key> DecodeMulti(I& in, I last, const Ctx& ctx);
template<typename Key, typename Ctx, typename I> inline NodeRef<Key> DecodeWrapped(I& in, I last, const Ctx& ctx);

//! Decode a list of script elements into a miniscript (except and_v, s:, and a:).
template<typename Key, typename Ctx, typename I>
inline NodeRef<Key> DecodeSingle(I& in, I last, const Ctx& ctx) {
    std::vector<NodeRef<Key>> subs;
    std::vector<Key> keys;
    int64_t k;
    static size_t counter = 0;
    ++counter;
    if (counter == 4) {
        printf("!\n");
    }

    if (last > in && in[0].first == OP_1) {
        ++in;
        return MakeNodeRef<Key>(NodeType::JUST_1);
    }
    if (last > in && in[0].first == OP_0) {
        ++in;
        return MakeNodeRef<Key>(NodeType::JUST_0);
    }
    if (last > in && in[0].second.size() == 33) {
        Key key;
        if (!ctx.FromPKBytes(in[0].second.begin(), in[0].second.end(), key)) throw compile_error("failed to retrieve key via PK bytes from context");
        ++in;
        return MakeNodeRef<Key>(NodeType::PK, Vector(std::move(key)));
    }
    // btcdeb symbolic keys {
    if (last > in && ctx.keymap.count(std::string(in[0].second.begin(), in[0].second.end()))) {
        Key key = ctx.keymap.at(std::string(in[0].second.begin(), in[0].second.end()));
        ++in;
        return MakeNodeRef<Key>(NodeType::PK, Vector(std::move(key)));
    }
    // }
    // btcdeb signature {
    if (ctx.fake_sigs.count(in[0].second) || (last > in && in[0].second.size() > 63 && in[-1].second.size() == 33)) {
        auto r = in[0].second;
        ++in;
        // auto sub = DecodeMulti<Key>(in, last, ctx);
        // if (!sub) throw compile_error("failed to decode expression");
        return MakeNodeRef<Key>(NodeType::SIG, /*Vector(std::move(sub)),*/ r);
    }
    // }
    if (last - in >= 5 && in[0].first == OP_VERIFY && in[1].first == OP_EQUAL && in[3].first == OP_HASH160 && in[4].first == OP_DUP && in[2].second.size() == 20) {
        Key key;
        if (!ctx.FromPKHBytes(in[2].second.begin(), in[2].second.end(), key)) throw compile_error("failed to retrieve key via PKH bytes from context");
        in += 5;
        return MakeNodeRef<Key>(NodeType::PK_H, Vector(std::move(key)));
    }
    if (last - in >= 2 && in[0].first == OP_CHECKSEQUENCEVERIFY && ParseScriptNumber(in[1], k)) {
        in += 2;
        if (k < 1 || k > 0x7FFFFFFFL) throw compile_error("k out of range 1..0x7FFFFFFF");
        return MakeNodeRef<Key>(NodeType::OLDER, k);
    }
    if (last - in >= 2 && in[0].first == OP_CHECKLOCKTIMEVERIFY && ParseScriptNumber(in[1], k)) {
        in += 2;
        if (k < 1 || k > 0x7FFFFFFFL) throw compile_error("k out of range 1..0x7FFFFFFF");
        return MakeNodeRef<Key>(NodeType::AFTER, k);
    }
    if (last - in >= 7 && in[0].first == OP_EQUAL && in[1].second.size() == 32 && in[2].first == OP_SHA256 && in[3].first == OP_VERIFY && in[4].first == OP_EQUAL && ParseScriptNumber(in[5], k) && k == 32 && in[6].first == OP_SIZE) {
        in += 7;
        return MakeNodeRef<Key>(NodeType::SHA256, in[-6].second);
    }
    if (last - in >= 7 && in[0].first == OP_EQUAL && in[1].second.size() == 20 && in[2].first == OP_RIPEMD160 && in[3].first == OP_VERIFY && in[4].first == OP_EQUAL && ParseScriptNumber(in[5], k) && k == 32 && in[6].first == OP_SIZE) {
        in += 7;
        return MakeNodeRef<Key>(NodeType::RIPEMD160, in[-6].second);
    }
    if (last - in >= 7 && in[0].first == OP_EQUAL && in[1].second.size() == 32 && in[2].first == OP_HASH256 && in[3].first == OP_VERIFY && in[4].first == OP_EQUAL && ParseScriptNumber(in[5], k) && k == 32 && in[6].first == OP_SIZE) {
        in += 7;
        return MakeNodeRef<Key>(NodeType::HASH256, in[-6].second);
    }
    if (last - in >= 7 && in[0].first == OP_EQUAL && in[1].second.size() == 20 && in[2].first == OP_HASH160 && in[3].first == OP_VERIFY && in[4].first == OP_EQUAL && ParseScriptNumber(in[5], k) && k == 32 && in[6].first == OP_SIZE) {
        in += 7;
        return MakeNodeRef<Key>(NodeType::HASH160, in[-6].second);
    }
    if (last - in >= 2 && in[0].first == OP_CHECKSIG) {
        ++in;
        auto sub = DecodeSingle<Key>(in, last, ctx);
        if (!sub) throw compile_error("failed to decode value for CHECKSIG");
        return MakeNodeRef<Key>(NodeType::WRAP_C, Vector(std::move(sub)));
    }
    if (last - in >= 3 && in[0].first == OP_BOOLAND) {
        ++in;
        auto sub1 = DecodeWrapped<Key>(in, last, ctx);
        if (!sub1) throw compile_error("failed to decode left expression for BOOLAND");
        auto sub2 = DecodeSingle<Key>(in, last, ctx);
        if (!sub2) throw compile_error("failed to decode right expression for BOOLAND");
        return MakeNodeRef<Key>(NodeType::AND_B, Vector(std::move(sub2), std::move(sub1)));
    }
    if (last - in >= 3 && in[0].first == OP_BOOLOR) {
        ++in;
        auto sub1 = DecodeWrapped<Key>(in, last, ctx);
        if (!sub1) throw compile_error("failed to decode left expression for BOOLOR");
        auto sub2 = DecodeSingle<Key>(in, last, ctx);
        if (!sub2) throw compile_error("failed to decode right expression for BOOLOR");
        return MakeNodeRef<Key>(NodeType::OR_B, Vector(std::move(sub2), std::move(sub1)));
    }
    if (last - in >= 2 && in[0].first == OP_VERIFY) {
        ++in;
        auto sub = DecodeSingle<Key>(in, last, ctx);
        if (!sub) throw compile_error("failed to decode expression");
        return MakeNodeRef<Key>(NodeType::WRAP_V, Vector(std::move(sub)));
    }
    if (last - in >= 2 && in[0].first == OP_0NOTEQUAL) {
        ++in;
        auto sub = DecodeSingle<Key>(in, last, ctx);
        if (!sub) throw compile_error("failed to decode expression");
        return MakeNodeRef<Key>(NodeType::WRAP_N, Vector(std::move(sub)));
    }
    if (last > in && in[0].first == OP_ENDIF) {
        ++in;
        if (last - in == 0) throw compile_error("last - in == 0");
        NodeRef<Key> sub1;
        sub1 = DecodeMulti<Key>(in, last, ctx);
        if (!sub1) throw compile_error("failed to decode expression");
        bool have_else = false;
        NodeRef<Key> sub2;
        if (last - in == 0) throw compile_error("last - in == 0");
        if (in[0].first == OP_ELSE) {
            ++in;
            have_else = true;
            sub2 = DecodeMulti<Key>(in, last, ctx);
            if (!sub2) throw compile_error("failed to decode multi expression");
        }
        if (last - in == 0 || (in[0].first != OP_IF && in[0].first != OP_NOTIF)) throw compile_error("last - in == 0 or not IF / NOTIF");
        bool negated = (in[0].first == OP_NOTIF);
        ++in;

        if (!have_else && !negated) {
            if (last > in && in[0].first == OP_DUP) {
                ++in;
                return MakeNodeRef<Key>(NodeType::WRAP_D, Vector(std::move(sub1)));
            }
            if (last - in >= 2 && in[0].first == OP_0NOTEQUAL && in[1].first == OP_SIZE) {
                in += 2;
                return MakeNodeRef<Key>(NodeType::WRAP_J, Vector(std::move(sub1)));
            }
            throw compile_error("exhausted non-negated no-else case");
        }
        if (have_else && negated) {
            auto sub3 = DecodeSingle<Key>(in, last, ctx);
            if (!sub3) throw compile_error("failed to decode expression");
            return MakeNodeRef<Key>(NodeType::ANDOR, Vector(std::move(sub3), std::move(sub1), std::move(sub2)));
        }
        if (!have_else && negated) {
            if (last - in >= 2 && in[0].first == OP_IFDUP) {
                ++in;
                auto sub3 = DecodeSingle<Key>(in, last, ctx);
                if (!sub3) throw compile_error("failed to decode expression");
                return MakeNodeRef<Key>(NodeType::OR_D, Vector(std::move(sub3), std::move(sub1)));
            }
            if (last > in) {
                auto sub3 = DecodeSingle<Key>(in, last, ctx);
                if (!sub3) throw compile_error("failed to decode expression");
                return MakeNodeRef<Key>(NodeType::OR_C, Vector(std::move(sub3), std::move(sub1)));
            }
            throw compile_error("exhausted negated no-else case");
        }
        if (have_else && !negated) {
            return MakeNodeRef<Key>(NodeType::OR_I, Vector(std::move(sub2), std::move(sub1)));
        }
        throw compile_error("exhausted OP_ENDIF variants");
    }
    keys.clear();
    if (last - in >= 3 && in[0].first == OP_CHECKMULTISIG) {
        int64_t n;
        if (!ParseScriptNumber(in[1], n)) throw compile_error("failed to parse script number");
        if (last - in < 3 + n) throw compile_error("last - in < 3 + n");
        if (n < 1 || n > 20) throw compile_error("multisig pubkey count out of range 1..20");
        for (int i = 0; i < n; ++i) {
            Key key;
            if (in[2 + i].second.size() != 33) throw compile_error("pubkey invalid size (must be 33)");
            if (!ctx.FromPKBytes(in[2 + i].second.begin(), in[2 + i].second.end(), key)) throw compile_error("unable to retrieve key from PK bytes via context");
            keys.push_back(std::move(key));
        }
        if (!ParseScriptNumber(in[2 + n], k)) throw compile_error("failed to parse script number");
        if (k < 1 || k > n) throw compile_error("k out of range 1..n");
        in += 3 + n;
        std::reverse(keys.begin(), keys.end());
        return MakeNodeRef<Key>(NodeType::THRESH_M, std::move(keys), k);
    }
    subs.clear();
    if (last - in >= 3 && in[0].first == OP_EQUAL && ParseScriptNumber(in[1], k)) {
        in += 2;
        while (last - in >= 2 && in[0].first == OP_ADD) {
            ++in;
            auto sub = DecodeWrapped<Key>(in, last, ctx);
            if (!sub) throw compile_error("failed to decode expression");
            subs.push_back(std::move(sub));
        }
        auto sub = DecodeSingle<Key>(in, last, ctx);
        if (!sub) throw compile_error("failed to decode expression");
        subs.push_back(std::move(sub));
        std::reverse(subs.begin(), subs.end());
        return MakeNodeRef<Key>(NodeType::THRESH, std::move(subs), k);
    }

    // this could be a signature

    throw compile_error("exhausted variants with no match (single)");
}

//! Decode a list of script elements into a miniscript (except a: and s:)
template<typename Key, typename Ctx, typename I>
inline NodeRef<Key> DecodeMulti(I& in, I last, const Ctx& ctx) {
    if (in == last) throw compile_error("empty input");
    auto sub = DecodeSingle<Key>(in, last, ctx);
    if (!sub) throw compile_error("failed to decode expression");
    while (in != last && in[0].first != OP_ELSE && in[0].first != OP_IF && in[0].first != OP_NOTIF && in[0].first != OP_TOALTSTACK && in[0].first != OP_SWAP) {
        auto sub2 = DecodeSingle<Key>(in, last, ctx);
        if (!sub2) throw compile_error("failed to decode expression");
        sub = MakeNodeRef<Key>(NodeType::AND_V, Vector(std::move(sub2), std::move(sub)));
    }
    return sub;
}

//! Decode a list of script elements into a miniscript (only a: and s:)
template<typename Key, typename Ctx, typename I>
inline NodeRef<Key> DecodeWrapped(I& in, I last, const Ctx& ctx) {
    if (last - in >= 3 && in[0].first == OP_FROMALTSTACK) {
        ++in;
        auto sub = DecodeMulti<Key>(in, last, ctx);
        if (!sub) throw compile_error("failed to decode multi expression");
        if (in == last || in[0].first != OP_TOALTSTACK) throw compile_error("in == last or OP_TOALTSTACK missing");
        ++in;
        return MakeNodeRef<Key>(NodeType::WRAP_A, Vector(std::move(sub)));
    }
    auto sub = DecodeMulti<Key>(in, last, ctx);
    if (!sub) throw compile_error("failed to decode multi expression");
    if (in == last || in[0].first != OP_SWAP) throw compile_error("in == last pr OP_SWAP missing");
    ++in;
    return MakeNodeRef<Key>(NodeType::WRAP_S, Vector(std::move(sub)));
}

} // namespace internal

template<typename Ctx>
inline NodeRef<typename Ctx::Key> FromString(const std::string& str, const Ctx& ctx) {
    using namespace internal;
    Span<const char> span = MakeSpan(str);
    auto ret = Parse<typename Ctx::Key>(span, ctx, 0);
    if (!ret || span.size()) throw compile_error("failed to parse data, or leftover data after parse");
    return ret;
}

template<typename Ctx>
inline NodeRef<typename Ctx::Key> FromScript(const CScript& script, const Ctx& ctx) {
    using namespace internal;
    std::vector<std::pair<opcodetype, std::vector<unsigned char>>> decomposed;
    if (!DecomposeScript(script, decomposed)) throw compile_error("failed to decompose script");
    auto it = decomposed.begin();
    auto ret = DecodeMulti<typename Ctx::Key>(it, decomposed.end(), ctx);
    if (!ret) throw compile_error("failed to decode multi expression");
    if (it != decomposed.end()) throw compile_error("leftover data after decode complete");
    return ret;
}

} // namespace miniscript

#endif // BITCOIN_SCRIPT_MINISCRIPT_H

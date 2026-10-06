#ifndef P2_BOUND_EXPR_H
#define P2_BOUND_EXPR_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <memory>
#include <string>

namespace exec {

/// A bound expression: every column reference is resolved to its
/// creation-order index and every operand type is checked against the
/// bound schema, so evaluation over a schema-conforming Tuple cannot
/// fail on lookup or type. The binder is the only writer: it stamps
/// every node with its static type as it binds bottom-up.
struct BoundExpr {
    enum class Kind { Literal, Column, Binary, Unary };
    Kind kind = Kind::Literal;

    // The node's static type, computed by the binder (Integer for
    // comparisons and boolean operations, the operand's type for
    // negation, the arithmetic ladder's result for arithmetic).
    tuple::ColumnType type = tuple::ColumnType::Integer;

    // Literal: the typed value (Integer, String, or Double Column).
    tuple::Column literal;

    // Column: the creation-order index into the bound schema (and so into
    // the row tuple).
    std::size_t index = 0;

    // Binary / Unary: the operation and its already-bound operands.
    int op = 0; // one of the Op constants below, per the node's shape
    std::unique_ptr<BoundExpr> left;   // Binary
    std::unique_ptr<BoundExpr> right;  // Binary
    std::unique_ptr<BoundExpr> operand; // Unary

    // Binary operations. Comparisons take same-typed operands and yield
    // Integer truth (1/0); arithmetic follows the type ladder (Double
    // whenever either side is Double, integer division truncates, Mod is
    // Integer-only); And/Or take Integer operands and yield Integer.
    enum BinaryOps {
        OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
        OP_AND, OP_OR,
        OP_PLUS, OP_MINUS, OP_MUL, OP_DIV, OP_MOD,
    };
    // Unary operations: negation (Integer/Double) and logical not (Integer).
    enum UnaryOps { OP_NEG, OP_NOT };
};

/// The static type the binder stamped on this node.
tuple::ColumnType bound_expr_type(const BoundExpr& expr);

/// Evaluates a bound expression over a schema-conforming row. The only
/// run-time failures are named errors: division or modulo by zero, and
/// integer overflow (an Integer operation whose mathematical result is
/// not representable in 32 bits, including negating INT_MIN). A binary
/// operation otherwise always yields a Column of the binder's computed
/// type. Comparison and arithmetic on Strings is lexicographic.
bool evaluate(const BoundExpr& expr, const tuple::Tuple& row,
              tuple::Column& out, std::string& error);

}

#endif // P2_BOUND_EXPR_H

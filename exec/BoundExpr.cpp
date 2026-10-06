#include "exec/BoundExpr.h"

#include <cmath>
#include <limits>

namespace exec {

namespace {

bool is_numeric(tuple::ColumnType type) {
    return type == tuple::ColumnType::Integer ||
           type == tuple::ColumnType::Double;
}

// Truth of a value in a boolean context: the binder only routes Integer
// columns here.
bool truth(const tuple::Column& column) {
    return std::get<int>(column.column_value()) != 0;
}

// Same-typed comparison, guaranteed by the binder.
bool compare(const tuple::Column& a, const tuple::Column& b, int op) {
    if (a.column_type() == tuple::ColumnType::String) {
        const std::string& x = std::get<std::string>(a.column_value());
        const std::string& y = std::get<std::string>(b.column_value());
        switch (op) {
            case BoundExpr::OP_EQ: return x == y;
            case BoundExpr::OP_NE: return x != y;
            case BoundExpr::OP_LT: return x < y;
            case BoundExpr::OP_LE: return x <= y;
            case BoundExpr::OP_GT: return x > y;
            default:               return x >= y;
        }
    }
    if (a.column_type() == tuple::ColumnType::Double) {
        const double x = std::get<double>(a.column_value());
        const double y = std::get<double>(b.column_value());
        switch (op) {
            case BoundExpr::OP_EQ: return x == y;
            case BoundExpr::OP_NE: return x != y;
            case BoundExpr::OP_LT: return x < y;
            case BoundExpr::OP_LE: return x <= y;
            case BoundExpr::OP_GT: return x > y;
            default:               return x >= y;
        }
    }
    const int x = std::get<int>(a.column_value());
    const int y = std::get<int>(b.column_value());
    switch (op) {
        case BoundExpr::OP_EQ: return x == y;
        case BoundExpr::OP_NE: return x != y;
        case BoundExpr::OP_LT: return x < y;
        case BoundExpr::OP_LE: return x <= y;
        case BoundExpr::OP_GT: return x > y;
        default:               return x >= y;
    }
}

// Numeric arithmetic; either side may be Double (the binder widened the
// Integer side before calling here only when the result is Double, so the
// integer path below runs with both operands Integer).
bool arith(int op, const tuple::Column& a, const tuple::Column& b,
           tuple::Column& out, std::string& error) {
    if (a.column_type() == tuple::ColumnType::Double ||
        b.column_type() == tuple::ColumnType::Double) {
        const double x = a.column_type() == tuple::ColumnType::Double
                             ? std::get<double>(a.column_value())
                             : static_cast<double>(std::get<int>(a.column_value()));
        const double y = b.column_type() == tuple::ColumnType::Double
                             ? std::get<double>(b.column_value())
                             : static_cast<double>(std::get<int>(b.column_value()));
        double r = 0.0;
        switch (op) {
            case BoundExpr::OP_PLUS:  r = x + y; break;
            case BoundExpr::OP_MINUS: r = x - y; break;
            case BoundExpr::OP_MUL:   r = x * y; break;
            case BoundExpr::OP_DIV:
                if (y == 0.0) {
                    error = "division by zero";
                    return false;
                }
                r = x / y;
                break;
            default:
                error = "operator '%' needs Integer operands";
                return false;
        }
        out = tuple::Column(r);
        return true;
    }
    // Widen to long long so + - * cannot overflow while computing; the
    // result is range-checked before narrowing back to int. Division and
    // modulo guard the zero divisor and the INT_MIN / -1 case (which has
    // no representable quotient).
    const long long x = std::get<int>(a.column_value());
    const long long y = std::get<int>(b.column_value());
    long long r = 0;
    switch (op) {
        case BoundExpr::OP_PLUS:  r = x + y; break;
        case BoundExpr::OP_MINUS: r = x - y; break;
        case BoundExpr::OP_MUL:   r = x * y; break;
        case BoundExpr::OP_DIV:
            if (y == 0) {
                error = "division by zero";
                return false;
            }
            if (x == std::numeric_limits<int>::min() && y == -1) {
                error = "integer overflow";
                return false;
            }
            r = x / y;
            break;
        case BoundExpr::OP_MOD:
            if (y == 0) {
                error = "modulo by zero";
                return false;
            }
            if (x == std::numeric_limits<int>::min() && y == -1) {
                error = "integer overflow";
                return false;
            }
            r = x % y;
            break;
        default:
            error = "malformed bound expression";
            return false;
    }
    if (r < std::numeric_limits<int>::min() || r > std::numeric_limits<int>::max()) {
        error = "integer overflow";
        return false;
    }
    out = tuple::Column(static_cast<int>(r));
    return true;
}

bool evaluate_node(const BoundExpr& expr, const tuple::Tuple& row,
                   tuple::Column& out, std::string& error) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            out = expr.literal;
            return true;
        case BoundExpr::Kind::Column:
            out = row.column(expr.index);
            return true;
        case BoundExpr::Kind::Binary: {
            tuple::Column left;
            tuple::Column right;
            if (!evaluate_node(*expr.left, row, left, error) ||
                !evaluate_node(*expr.right, row, right, error)) {
                return false;
            }
            switch (expr.op) {
                case BoundExpr::OP_EQ:
                case BoundExpr::OP_NE:
                case BoundExpr::OP_LT:
                case BoundExpr::OP_LE:
                case BoundExpr::OP_GT:
                case BoundExpr::OP_GE:
                    out = tuple::Column(compare(left, right, expr.op) ? 1 : 0);
                    return true;
                case BoundExpr::OP_AND:
                    out = tuple::Column(truth(left) && truth(right) ? 1 : 0);
                    return true;
                case BoundExpr::OP_OR:
                    out = tuple::Column(truth(left) || truth(right) ? 1 : 0);
                    return true;
                default:
                    return arith(expr.op, left, right, out, error);
            }
        }
        case BoundExpr::Kind::Unary: {
            tuple::Column value;
            if (!evaluate_node(*expr.operand, row, value, error)) {
                return false;
            }
            if (expr.op == BoundExpr::OP_NOT) {
                out = tuple::Column(truth(value) ? 0 : 1);
                return true;
            }
            if (value.column_type() == tuple::ColumnType::Double) {
                out = tuple::Column(-std::get<double>(value.column_value()));
            } else {
                const int value_int = std::get<int>(value.column_value());
                if (value_int == std::numeric_limits<int>::min()) {
                    error = "integer overflow";
                    return false;
                }
                out = tuple::Column(-value_int);
            }
            return true;
        }
    }
    error = "malformed bound expression";
    return false;
}

}

tuple::ColumnType bound_expr_type(const BoundExpr& expr) {
    return expr.type;
}

bool evaluate(const BoundExpr& expr, const tuple::Tuple& row,
              tuple::Column& out, std::string& error) {
    error.clear();
    return evaluate_node(expr, row, out, error);
}

}

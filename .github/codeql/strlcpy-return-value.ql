/**
 * @kind problem
 */

import cpp
import semmle.code.cpp.valuenumbering.GlobalValueNumbering
import semmle.code.cpp.valuenumbering.HashCons
import semmle.code.cpp.dataflow.DataFlow
import semmle.code.cpp.controlflow.Dominance

class StrlcpyCall extends FunctionCall {
  StrlcpyCall() { getTarget().getName().matches("%strlcpy") }
}

from StrlcpyCall call, Expr use
where
  DataFlow::localExprFlow(call, use) and

  // Not interesting because the value is discarded.
  not use instanceof ExprInVoidContext and

  // Not interesting because the value is just copied to another local variable.
  not exists(AssignExpr assign |
    use = assign.getRValue() and
    assign.getLValue().(VariableAccess).getTarget() instanceof LocalScopeVariable
  ) and

  // Not interesting because the value is just used in a comparison (<, <=, >, >=).
  not exists(RelationalOperation relop | relop.getAnOperand() = use) and

  // Not interesting because the use is dominated by a comparison operation that
  // (probably) checks for overflow. This logic does not check that the overflow
  // check is perfect. It gives the developer the benefit of the doubt as long
  // as it looks like they have made some kind of effort towards bounds checking.
  not exists(RelationalOperation relop |
    globalValueNumber(relop.getAnOperand()) = globalValueNumber(call) and
    (
      globalValueNumber(relop.getAnOperand()) = globalValueNumber(call.getArgument(2)) or

      // Hack to make it work on github/git. (They're using global variables for parameter passing.)
      hashCons(relop.getAnOperand()) = hashCons(call.getArgument(2))
    ) and
    dominates(relop, use)
  )
select call, "Result of strlcpy is used without checking for overflow."

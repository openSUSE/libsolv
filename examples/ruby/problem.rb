#
# Extend SatSolver::Problem with to_s
#

class Satsolverx::Problem
  def to_s
    case self.reason
      when SatSolver::SOLVER_PROBLEM_UPDATE_RULE #1
	reason = "problem with installed"
      when SatSolver::SOLVER_PROBLEM_JOB_RULE #2
	reason = "conflicting requests"
      when SatSolver::SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP #3
	reason = "nothing provides requested"
      when SatSolver::SOLVER_PROBLEM_NOT_INSTALLABLE #4
	reason = "not installable"
      when SatSolver::SOLVER_PROBLEM_NOTHING_PROVIDES_DEP #5
	reason = "nothing provides rel required by source"
      when SatSolver::SOLVER_PROBLEM_SAME_NAME #6
	reason = "cannot install both"
      when SatSolver::SOLVER_PROBLEM_PACKAGE_CONFLICT #7
	reason = "source conflicts with rel provided by target"
      when SatSolver::SOLVER_PROBLEM_PACKAGE_OBSOLETES #8
	reason = "source obsoletes rel provided by target"
      when SatSolver::SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE #9
	reason = "source requires rel but no providers are installable"
      else
	reason = "**unknown**"
    end
    "[#{self.reason}]: #{reason}] Source #{self.source}, Rel #{self.relation}, Target #{self.target}"
  end
end

#
# Extend SatSolver::Decision with to_s
#

class Satsolverx::Decision
  def to_s
    case self.op
      when SatSolver::DEC_INSTALL
        return "Install #{self.solvable} #{self.reason}"
      when SatSolver::DEC_REMOVE
	return "Remove #{self.solvable} #{self.reason}"
      when SatSolver::DEC_OBSOLETE
	return "Obsolete #{self.solvable} #{self.reason}"
      when SatSolver::DEC_UPDATE
	return "Update #{self.solvable} #{self.reason}"
      else
	return "Decision op #{self.op}"
      end
    "**Decision**"
  end
end

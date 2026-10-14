// Ghidra headless script: list every instruction whose memory operand uses one of the given
// displacements (e.g. [rbx+0x21d8]), with its function, so fields can be traced to their writers
// without decompiling the whole program. Needs an analysed program (instructions must exist).
//
//   ... -postScript FindDisplacement.java <outFile> <disp> [<disp> ...]
//
// Displacements are hex. A line is marked "W" when the operand with the displacement is the
// instruction's first operand (the destination for mov-like instructions), "R" otherwise.
//@category BG3VR

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindDisplacement extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: FindDisplacement.java <outFile> <disp> [<disp> ...]");
            return;
        }
        Set<Long> wanted = new HashSet<>();
        for (int i = 1; i < args.length; ++i) {
            wanted.add(Long.parseLong(args[i].replace("0x", ""), 16));
        }
        long base = currentProgram.getImageBase().getOffset();
        int hits = 0;
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Instruction insn = it.next();
                for (int op = 0; op < insn.getNumOperands(); ++op) {
                    int type = insn.getOperandType(op);
                    if ((type & OperandType.DYNAMIC) == 0) {
                        continue; // not a memory operand with a base register
                    }
                    boolean match = false;
                    for (Object o : insn.getOpObjects(op)) {
                        if (o instanceof Scalar && wanted.contains(((Scalar) o).getUnsignedValue())) {
                            match = true;
                        }
                    }
                    if (!match) {
                        continue;
                    }
                    Function f = getFunctionContaining(insn.getAddress());
                    String fn = f == null ? "?" : String.format("0x%X", f.getEntryPoint().getOffset() - base);
                    out.printf("0x%X  %s  fn %s  %s%n", insn.getAddress().getOffset() - base, op == 0 ? "W" : "R",
                               fn, insn.toString());
                    ++hits;
                }
            }
            out.printf("// %d hit(s)%n", hits);
        }
        println("FindDisplacement: " + hits + " hit(s) -> " + args[0]);
    }
}

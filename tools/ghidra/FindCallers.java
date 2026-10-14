// Ghidra headless script: list every reference to the given functions (RVAs, hex) with the
// referencing instruction and its containing function. Needs an analysed program.
//
//   ... -postScript FindCallers.java <outFile> <rva> [<rva> ...]
//@category BG3VR

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class FindCallers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                Address target = currentProgram.getImageBase().add(Long.parseLong(args[i].replace("0x", ""), 16));
                out.printf("== 0x%X%n", target.getOffset() - base);
                for (Reference ref : getReferencesTo(target)) {
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    Instruction insn = getInstructionAt(from);
                    out.printf("  0x%X  %s  fn %s  %s%n", from.getOffset() - base, ref.getReferenceType(),
                               f == null ? "?" : String.format("0x%X", f.getEntryPoint().getOffset() - base),
                               insn == null ? "(data)" : insn.toString());
                }
            }
        }
    }
}

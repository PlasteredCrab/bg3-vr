// Ghidra headless script: for each address (RVA, hex), find the function that contains it in the
// analysed program and write its decompiled C, marking the line nearest the address. For stack
// addresses from logs (return addresses), so the caller's code around the call can be read.
//
//   ... -postScript DecompileContaining.java <outFile> <rva> [<rva> ...]
//@category BG3VR

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class DecompileContaining extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long base = currentProgram.getImageBase().getOffset();
        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                Address at = currentProgram.getImageBase().add(Long.parseLong(args[i].replace("0x", ""), 16));
                Function f = getFunctionContaining(at);
                out.printf("// ===== rva 0x%X =====%n", at.getOffset() - base);
                // The call before a return address.
                Instruction previous = getInstructionBefore(at);
                if (previous != null) {
                    out.printf("// instruction before: 0x%X %s%n", previous.getAddress().getOffset() - base, previous);
                }
                if (f == null) {
                    out.println("// no function contains it");
                    continue;
                }
                out.printf("// in function 0x%X %s%n", f.getEntryPoint().getOffset() - base, f.getName());
                DecompileResults results = decompiler.decompileFunction(f, 120, monitor);
                out.println(results.decompileCompleted() ? results.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
    }
}

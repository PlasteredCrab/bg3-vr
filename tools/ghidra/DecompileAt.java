// Ghidra headless script: decompile the functions at the given RVAs of the imported exe into one
// text file, without a full auto-analysis (each function is created from its address first).
//
//   analyzeHeadless <projectDir> <project> -process <program> -noanalysis -readOnly
//       -scriptPath tools/ghidra -postScript DecompileAt.java <outFile> <rva> [<rva> ...]
//
// RVAs are hex, as in docs/offsets.md (relative to the image base). Run by tools/ghidra/decompile.ps1.
//@category BG3VR

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileAt.java <outFile> <rva> [<rva> ...]");
            return;
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);
        Address base = currentProgram.getImageBase();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; ++i) {
                String text = args[i].toLowerCase().replace("0x", "");
                Address address = base.add(Long.parseLong(text, 16));
                Function function = getFunctionContaining(address);
                if (function == null) {
                    // No analysis was run: create the function from its entry (the RVA should be one).
                    new CreateFunctionCmd(address).applyTo(currentProgram, monitor);
                    function = getFunctionAt(address);
                }
                out.println("// ===== rva 0x" + text.toUpperCase() + " =====");
                if (function == null) {
                    out.println("// no function could be created here");
                    continue;
                }
                DecompileResults results = decompiler.decompileFunction(function, 120, monitor);
                if (results.decompileCompleted()) {
                    out.println(results.getDecompiledFunction().getC());
                } else {
                    out.println("// decompile failed: " + results.getErrorMessage());
                }
            }
        }
        decompiler.dispose();
    }
}

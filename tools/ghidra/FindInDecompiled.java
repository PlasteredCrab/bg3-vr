// Ghidra headless script: decompile every function whose entry lies in [start, end) (RVAs) and list
// the ones whose C contains ALL the given substrings, with the matching lines.
//
//   ... -postScript FindInDecompiled.java <outFile> <startRva> <endRva> <substring> [<substring> ...]
//
// RVAs are hex. Used to find code by the fields it touches (e.g. "+ 0x58)" and "+ 100)").
// Ghidra's batch launcher mangles spaces and parentheses in arguments, so in substrings "_" stands for
// a space, "~" for ")" and "#" for "|": pass "+_0x58~" for "+ 0x58)".
//@category BG3VR

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class FindInDecompiled extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) {
            println("usage: FindInDecompiled.java <outFile> <startRva> <endRva> <substring> [...]");
            return;
        }
        for (int i = 3; i < args.length; ++i) {
            args[i] = args[i].replace('_', ' ').replace('~', ')').replace('#', '|');
        }
        Address base = currentProgram.getImageBase();
        Address start = base.add(Long.parseLong(args[1].replace("0x", ""), 16));
        Address end = base.add(Long.parseLong(args[2].replace("0x", ""), 16));
        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);
        int scanned = 0, matched = 0;
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(start, true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                if (function.getEntryPoint().compareTo(end) >= 0) {
                    break;
                }
                ++scanned;
                DecompileResults results = decompiler.decompileFunction(function, 60, monitor);
                if (!results.decompileCompleted()) {
                    continue;
                }
                String c = results.getDecompiledFunction().getC();
                boolean all = true;
                for (int i = 3; i < args.length && all; ++i) {
                    all = c.contains(args[i]);
                }
                if (!all) {
                    continue;
                }
                ++matched;
                long rva = function.getEntryPoint().subtract(base);
                out.println("// ===== rva 0x" + Long.toHexString(rva).toUpperCase() + " " + function.getName() + " =====");
                for (String line : c.split("\n")) {
                    for (int i = 3; i < args.length; ++i) {
                        if (line.contains(args[i])) {
                            out.println("  " + line.trim());
                            break;
                        }
                    }
                }
            }
            out.println("// scanned " + scanned + " functions, matched " + matched);
        }
        decompiler.dispose();
    }
}

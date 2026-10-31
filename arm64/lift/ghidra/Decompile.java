// usage: -postScript Decompile.java <function name> <out file>
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class Decompile extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(a[1]))) {
            for (Function f : currentProgram.getListing().getFunctions(true)) {
                if (!java.util.Arrays.asList(a[0].split(",")).contains(f.getName())) continue;
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                DecompileResults r = d.decompileFunction(f, 300, monitor);
                w.println(r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : r.getErrorMessage());
            }
        }
    }
}

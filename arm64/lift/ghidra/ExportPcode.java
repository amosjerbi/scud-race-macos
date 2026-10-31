// Ghidra headless script: dump the p-code of every instruction of every function, plus indirect-branch targets.
// usage: -postScript ExportPcode.java <out file>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportPcode extends GhidraScript {
    private String vn(Varnode v) {
        if (v == null) return "-";
        Address a = v.getAddress();
        String sp = a.getAddressSpace().getName();
        char c = sp.equals("register") ? 'r' : sp.equals("unique") ? 'u' : sp.equals("const") ? 'c' : sp.equals("ram") ? 'm' : '?';
        if (c == '?') c = sp.charAt(0);
        return c + ":" + Long.toHexString(a.getOffset()) + ":" + v.getSize();
    }
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        Language lang = currentProgram.getLanguage();
        Listing lst = currentProgram.getListing();
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out), 1 << 20))) {
            FunctionIterator fi = lst.getFunctions(true);
            int nf = 0, ni = 0;
            while (fi.hasNext() && !monitor.isCancelled()) {
                Function f = fi.next();
                if (f.isExternal() || f.isThunk() && false) continue;
                AddressSetView body = f.getBody();
                w.println("F " + Long.toHexString(f.getEntryPoint().getOffset()) + " " + f.getName().replace(' ', '_'));
                InstructionIterator ii = lst.getInstructions(body, true);
                while (ii.hasNext()) {
                    Instruction in = ii.next();
                    w.println("I " + Long.toHexString(in.getAddress().getOffset()) + " " + in.getLength() + " " + in.getMnemonicString());
                    if (in.getFlowType().isComputed() && in.getFlowType().isJump()) {
                        StringBuilder sb = new StringBuilder("J");
                        for (Reference r : in.getReferencesFrom())
                            if (r.getReferenceType().isJump() && r.getReferenceType().isComputed()) sb.append(' ').append(Long.toHexString(r.getToAddress().getOffset()));
                        w.println(sb);
                    }
                    PcodeOp[] ops = in.getPcode();
                    for (PcodeOp op : ops) {
                        StringBuilder sb = new StringBuilder("O ").append(op.getMnemonic()).append(' ').append(vn(op.getOutput()));
                        for (int k = 0; k < op.getNumInputs(); k++) sb.append(' ').append(vn(op.getInput(k)));
                        if (op.getOpcode() == PcodeOp.CALLOTHER) sb.append(" #").append(lang.getUserDefinedOpName((int) op.getInput(0).getOffset()));
                        w.println(sb);
                    }
                    ni++;
                }
                w.println("E");
                nf++;
            }
            w.println("# functions " + nf + " instructions " + ni);
            println("exported " + nf + " functions, " + ni + " instructions");
        }
    }
}

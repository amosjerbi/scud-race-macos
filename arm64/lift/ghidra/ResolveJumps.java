// Ghidra headless script, run before ExportPcode: resolve the gcc jump tables Ghidra gave up on (the game's recompiled
// PowerPC dispatchers have tables of thousands of entries), disassemble their targets and grow the function bodies.
// Pattern: [cmp idx, N; ja default] lea base, [rip+table]; movsxd t, dword [base + idx*4]; add t, base; jmp t
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
import java.util.*;

public class ResolveJumps extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing lst = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        int total = 0, tables = 0;
        for (int pass = 0; pass < 8; pass++) {
            List<Instruction> todo = new ArrayList<>();
            InstructionIterator it = lst.getInstructions(text.getStart(), true);
            while (it.hasNext()) {
                Instruction in = it.next();
                if (!text.contains(in.getAddress())) break;
                if (!in.getFlowType().isComputed() || !in.getFlowType().isJump()) continue;
                boolean has = false;
                for (Reference r : in.getReferencesFrom()) if (r.getReferenceType().isFlow()) has = true;
                if (!has) todo.add(in);
            }
            if (todo.isEmpty()) break;
            int found = 0;
            for (Instruction jmp : todo) {
                Address table = null; long bound = -1;
                Instruction p = jmp;
                for (int k = 0; k < 16 && p != null; k++) {
                    p = p.getPrevious();
                    if (p == null) break;
                    String m = p.getMnemonicString();
                    if (table == null && m.equals("LEA")) {
                        for (Reference r : p.getReferencesFrom()) { Address a = r.getToAddress(); if (a.isMemoryAddress() && !text.contains(a) && mem.getBlock(a) != null && mem.getBlock(a).isInitialized()) table = a; }
                    }
                    if (table != null && m.equals("CMP") && p.getNumOperands() == 2) {
                        Object[] o = p.getOpObjects(1);
                        if (o.length == 1 && o[0] instanceof Scalar) { bound = ((Scalar) o[0]).getUnsignedValue(); break; }
                    }
                }
                if (table == null) { println("unresolved jump at " + jmp.getAddress() + ": no table"); continue; }
                long n = bound >= 0 ? bound + 1 : 100000;
                Set<Address> tg = new LinkedHashSet<>();
                for (long i = 0; i < n; i++) {
                    Address e = table.add(4 * i);
                    if (!mem.contains(e) || !mem.getBlock(e).isInitialized()) break;
                    int d = mem.getInt(e);
                    Address t = table.add(d);
                    if (!text.contains(t)) { if (bound < 0) break; continue; }
                    tg.add(t);
                }
                if (tg.isEmpty()) { println("unresolved jump at " + jmp.getAddress() + ": empty table at " + table); continue; }
                for (Address t : tg) {
                    jmp.addOperandReference(0, t, RefType.COMPUTED_JUMP, SourceType.ANALYSIS);
                    if (lst.getInstructionAt(t) == null) new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
                }
                Function f = getFunctionContaining(jmp.getAddress());
                if (f != null) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                println("jump at " + jmp.getAddress() + ": table " + table + ", " + tg.size() + " targets" + (bound >= 0 ? "" : " (no bound found)"));
                found++; total += tg.size(); tables++;
            }
            if (found == 0) break;
        }
        println("ResolveJumps: " + tables + " tables, " + total + " targets");
    }
}

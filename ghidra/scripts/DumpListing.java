// Print the disassembly listing of address ranges.
// Usage: DumpListing.java <outFile> <start> <end> [<start> <end> ...]
//@category LUMINKEY
import java.io.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class DumpListing extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = getScriptArgs();
		try (PrintWriter w = new PrintWriter(new FileWriter(a[0], true))) {
			for (int i = 1; i + 1 < a.length; i += 2) {
				AddressSet set = new AddressSet(toAddr(Long.decode(a[i])), toAddr(Long.decode(a[i + 1])));
				w.println("==== " + a[i] + " .. " + a[i + 1]);
				CodeUnitIterator it = currentProgram.getListing().getCodeUnits(set, true);
				while (it.hasNext()) {
					CodeUnit cu = it.next();
					Symbol s = getSymbolAt(cu.getAddress());
					if (s != null) {
						w.println(s.getName() + ":");
					}
					StringBuilder refs = new StringBuilder();
					for (Reference r : cu.getReferencesFrom()) {
						if (r.getReferenceType().isData()) {
							refs.append(" ->").append(r.getToAddress());
						}
					}
					w.printf("  %s  %-40s%s%n", cu.getAddress(), cu.toString(), refs);
				}
			}
		}
	}
}

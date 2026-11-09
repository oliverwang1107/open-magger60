// Pre-analysis setup for the Magger60 32 KB bootloader dumped from a real keyboard (backup1/bootloader_0x08000000.bin).
// Import the raw image at 0x08000000 with ARM:LE:32:Cortex, then run this as -preScript. It adds SRAM and
// peripheral blocks, types the vector table and seeds a Thumb function at every handler.
//@category LUMINKEY
import java.util.*;

import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

public class BootPreSetup extends GhidraScript {

	static final long BASE = 0x08000000L;
	static final long SIZE = 0x8000L;
	static final long SRAM_BASE = 0x20000000L;
	static final long SRAM_SIZE = 0x28000;

	Memory mem;
	SymbolTable st;

	@Override
	public void run() throws Exception {
		mem = currentProgram.getMemory();
		st = currentProgram.getSymbolTable();
		MemoryBlock img = mem.getBlock(addr(BASE));
		img.setName("boot_flash");
		img.setPermissions(true, false, true);

		MemoryBlock ram = mem.createInitializedBlock("sram", addr(SRAM_BASE), SRAM_SIZE, (byte) 0, monitor, false);
		ram.setPermissions(true, true, false);
		uninit("flash_app", 0x08008000L, 0x38000L, true, true, false, true);
		uninit("PERIPH", 0x40000000L, 0x20000000L, true, true, false, true);
		uninit("SCS", 0xE0000000L, 0x100000L, true, true, false, true);

		st.createLabel(addr(BASE), "__Vectors", SourceType.IMPORTED);
		int n = 16;
		while (n < 128) {
			long v = u32(BASE + 4L * n);
			if (v != 0 && (v < BASE || v >= BASE + SIZE || (v & 1) == 0)) {
				break;
			}
			n++;
		}
		println("vector table: " + n + " entries");
		Set<Long> seen = new TreeSet<>();
		for (int i = 0; i < n; i++) {
			createData(addr(BASE + 4L * i), PointerDataType.dataType);
			long v = u32(BASE + 4L * i);
			if (i == 0 || v == 0) {
				continue;
			}
			seen.add(v & ~1L);
		}
		for (long t : seen) {
			Address a = addr(t);
			Register tmode = currentProgram.getRegister("TMode");
			currentProgram.getProgramContext().setValue(tmode, a, a, java.math.BigInteger.ONE);
			new ArmDisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
			createFunction(a, t == (u32(BASE + 4) & ~1L) ? "Reset_Handler" : null);
		}
		st.addExternalEntryPoint(addr(u32(BASE + 4) & ~1L));
	}

	Address addr(long a) {
		return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
	}

	long u32(long a) throws Exception {
		return mem.getInt(addr(a)) & 0xffffffffL;
	}

	void uninit(String name, long start, long len, boolean r, boolean w, boolean x, boolean vol) throws Exception {
		MemoryBlock b = mem.createUninitializedBlock(name, addr(start), len, false);
		b.setPermissions(r, w, x);
		b.setVolatile(vol);
	}
}

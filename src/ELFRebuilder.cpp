#include "exutil.h"
#include "ELFRebuilder.h"
#include "Log.h"
#include <cstdlib>

ELFRebuilder::ELFRebuilder(ELFReader &_reader, bool _force)
	: reader(_reader), force(_force){
		
	elf_header = reader.getElfHeader();
	phdr_table = reader.getPhdrTable();

}

ELFRebuilder::~ELFRebuilder(){
	if(rebuild_data != NULL){
		delete [](uint8_t*)rebuild_data;
	}
}


bool ELFRebuilder::rebuild(){
	if(force || reader.getDamageLevel() == 2){
		if(!reader.isLoad()) reader.load();
		return totalRebuild();
	} else if(reader.getDamageLevel() == 1){
		return simpleRebuild() && rebuildData();
	}
	return false;
}


/**
 * Just repair the section headers address and offset.
 * This function is just like the checkSectionHeader in ELFReader.
 * Calling this function. We assume that the so-file have valid 
 * program header, elf header, and valid size of each section. 
 * The all thing that this file need is section offset and address.
 */
bool ELFRebuilder::simpleRebuild(){
	VLOG("Starting repair the section.");
	rebuild_size = sizeof(Elf_Ehdr) + reader.getPhdrSize() + reader.getMidPartSize() + reader.getShdrSize();
	Elf_Shdr *shdr_table = reader.getShdrTable();
	Elf_Phdr *phdr_table = reader.getPhdrTable();
	int shdr_num = reader.getShdrNum();
	int phdr_num = reader.getPhdrNum();

	int loadIndex[2] = {-1, -1};
	for(int i=0, j=0;i<phdr_num;i++){
		if(phdr_table[i].p_type == PT_LOAD){
			loadIndex[j++] = i;
		}
	}

	int firstAddress = sizeof(Elf_Ehdr) + reader.getPhdrSize();
	// build the first section.
	shdr_table[1].sh_addr = shdr_table[1].sh_offset = firstAddress;
	
	
	int i;
	DLOG("Start repair the section mapping at first LOAD segment.");
	for(i=2;i<shdr_num;i++){	//we have already check 2 section. So "i" start at 2.
		
		Elf_Addr curAddr = shdr_table[i-1].sh_addr + shdr_table[i-1].sh_size;
		Elf_Off curOffset = shdr_table[i-1].sh_offset + shdr_table[i-1].sh_size;
		// bits align
		while(curAddr & (shdr_table[i].sh_addralign-1)) { curAddr++; }
		while(curOffset & (shdr_table[i].sh_addralign-1)) {curOffset++;}
		
		if(curOffset >= phdr_table[loadIndex[0]].p_filesz + phdr_table[loadIndex[0]].p_offset) { break; }
		shdr_table[i].sh_addr = curAddr;
		shdr_table[i].sh_offset = curOffset;	
		
	}
	
	// Rebuild the second LOAD segment
	// First get the second LOAD segment address and offset
	DLOG("Start repair the section mapping at second LOAD segment.");
	shdr_table[i].sh_addr = phdr_table[loadIndex[1]].p_vaddr;
	shdr_table[i].sh_offset = phdr_table[loadIndex[1]].p_offset;

	for(i=i+1;i<shdr_num;i++){
		
		Elf_Addr curAddr = shdr_table[i-1].sh_addr + shdr_table[i-1].sh_size;
		Elf_Off curOffset = shdr_table[i-1].sh_offset + shdr_table[i-1].sh_size;
		
		Elf_Word align = shdr_table[i].sh_addralign;
		//bits align
		//It looks like .got section align 8. But record 4 in it section header. No idea.
		// I figure out it is .got section by check the previous section.
		// Because .got section always follow .dynamic section, that what i do.
		if(shdr_table[i-1].sh_type == SHT_DYNAMIC){
			align = 8;
		}
		while(curAddr & (align-1)) { curAddr++; }
		//specific situation
		if(shdr_table[i].sh_type == SHT_NOBITS) align = 4;
		while(curOffset & (align-1)) {curOffset++;}
		
		shdr_table[i].sh_addr = curAddr;
		shdr_table[i].sh_offset = curOffset;
		// Beside Load segment, break
		if(curOffset >= phdr_table[loadIndex[1]].p_filesz + phdr_table[loadIndex[1]].p_offset) { break; }
	}

	// The remain section won't be load. So the address is 0.
	// And the .bss section type is SHT_NOBITS.
	// So the follow section offset is the same as it.
	DLOG("Repair the not LOAD section.");
	i++;
	shdr_table[i].sh_addr = 0;
	shdr_table[i].sh_offset = shdr_table[i-1].sh_offset;	
	for(i=i+1;i<shdr_num;i++){
		Elf_Off curOffset = shdr_table[i-1].sh_offset + shdr_table[i-1].sh_size;
		while(curOffset & (shdr_table[i].sh_addralign-1)) {curOffset++;}
		
		shdr_table[i].sh_addr = 0;
		shdr_table[i].sh_offset = curOffset;
	}

	DLOG("Repair finish.");
	return true;
}

bool ELFRebuilder::rebuildData(){
	rebuild_data = new uint8_t[rebuild_size];
	DLOG("Copy elf header data. Elf header size = %d", sizeof(elf_header));
	uint8_t *tmp = rebuild_data;
	memcpy(tmp, &elf_header, sizeof(elf_header));
	tmp += sizeof(elf_header);

	size_t phdr_size = reader.getPhdrSize();
	DLOG("Copy program header data. Program header size = %d", phdr_size);
	memcpy(tmp, phdr_table, phdr_size);
	tmp += phdr_size;

	size_t midPart_size = reader.getMidPartSize();
	uint8_t* midPart = reinterpret_cast<uint8_t*>(reader.getMidPart());
	DLOG("Copy midPart data. MidPart size = %d", midPart_size);
	memcpy(tmp, midPart, midPart_size);
	tmp += midPart_size;
	
	size_t shdr_size = reader.getShdrSize();
	DLOG("Copy section header data. Section header size = %d", shdr_size);
	memcpy(tmp, reader.getShdrTable(), shdr_size);

	DLOG("rebuild_data prepared.");
	return false;
}

bool ELFRebuilder::totalRebuild(){
	VLOG("Using plan B to rebuild the section.");
	if(rebuildPhdr() && readSoInfo() && rebuildShdr() && rebuildRelocs() &&	rebuildFinish()){
		return true;
	}
	ELOG("Using plan B to rebuild failed.");
	exit(EXIT_FAILURE);
}

/**
 * Expend file size to memory size. Because we will dump them 
 * all from memory. After rebuild, the load size of the file 
 * is exactly the memory size. We don't need to padding for the 
 * page align. Because the data of the file is already align with 
 * page.
 */ 
bool ELFRebuilder::rebuildPhdr(){
	Elf_Phdr* phdr = (Elf_Phdr *)reader.getLoadedPhdr();
	for(int i=0;i<reader.getPhdrNum();i++){
		phdr[i].p_filesz = phdr[i].p_memsz;
		// p_paddr and p_align is not used in load, ignore it.
		phdr[i].p_paddr = phdr[i].p_vaddr;
		phdr[i].p_offset = phdr[i].p_vaddr;
	}
	DLOG("Program header rebuild finish.");
	return true;
}

bool ELFRebuilder::readSoInfo(){
	si.name = reader.getFileName();
	si.base = si.load_bias = reader.getLoadBias();
	si.phdr = reader.getPhdrTable();
	si.phnum = reader.getPhdrNum();

	Elf_Addr base = si.base;
	phdr_table_get_load_size(si.phdr, si.phnum, &si.min_load, &si.max_load, &si.loadSegEnd);

	// get .dynamic table
	phdr_table_get_dynamic_section(si.phdr, si.phnum, si.load_bias, &si.dynamic, &si.dynamic_count, &si.dynamic_flags);

	phdr_table_get_interpt_section(si.phdr, si.phnum, si.load_bias, &si.interp, &si.interp_size);

	if(si.dynamic == NULL){
		ELOG("dynamic section unavailable. Cannot rebuild.");
		return false;
	}
	//get .arm_exidx
	phdr_table_get_arm_exidx(si.phdr, si.phnum, si.base, &si.ARM_exidx, &si.ARM_exidx_count);

	// scan the dynamic section and get useful information.
	uint32_t needed_count = 0;
	for(Elf_Dyn* dyn = si.dynamic;dyn->d_tag != DT_NULL;dyn++){
		switch(dyn->d_tag){
			case DT_HASH:
				si.hash = dyn->d_un.d_ptr + base;
				si.nbucket = ((unsigned *)si.hash)[0];
				si.nchain = ((unsigned *)si.hash)[1];
				si.bucket = (unsigned *)si.hash + 8;
				si.chain = (unsigned *)si.bucket + 4*si.nbucket;
				break;
			case DT_STRTAB:
				si.strtab = (const char*)(dyn->d_un.d_ptr + base);
				VLOG("string table found at %x", dyn->d_un.d_ptr);
				break;
			case DT_SYMTAB:
				si.symtab = (Elf_Sym *) (dyn->d_un.d_ptr + base);
				VLOG("symbol table found at %x", dyn->d_un.d_ptr);
				break;
			case DT_PLTREL:
				if (dyn->d_un.d_val != DT_REL) {
					VLOG("unsupported DT_RELA in \"%s\"", si.name);
					return false;
				}
				break;
			case DT_JMPREL:
				si.plt_rel = (Elf_Rel*) (dyn->d_un.d_ptr + base);
				VLOG("%s plt_rel (DT_JMPREL) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_PLTRELSZ:
				si.plt_rel_count = dyn->d_un.d_val / sizeof(Elf_Rel);
				VLOG("%s plt_rel_count (DT_PLTRELSZ) %d", si.name, si.plt_rel_count);
				break;
			case DT_REL:
				si.rel = (Elf_Rel*) (dyn->d_un.d_ptr + base);
				VLOG("%s rel (DT_REL) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_RELSZ:
				si.rel_count = dyn->d_un.d_val / sizeof(Elf_Rel);
				VLOG("%s rel_size (DT_RELSZ) %d", si.name, si.rel_count);
				break;
			case DT_PLTGOT:
				/* Save this in case we decide to do lazy binding. We don't yet. */
				si.plt_got = (Elf_Addr *)(dyn->d_un.d_ptr + base);
				break;
			case DT_DEBUG:
				// Set the DT_DEBUG entry to the address of _r_debug for GDB
				// if the dynamic table is writable
				break;
			case DT_RELA:
				VLOG("unsupported DT_RELA in \"%s\"", si.name);
				return false;
			case DT_INIT:
				si.init_func = reinterpret_cast<void*>(dyn->d_un.d_ptr + base);
				VLOG("%s constructors (DT_INIT) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_FINI:
				si.fini_func = reinterpret_cast<void*>(dyn->d_un.d_ptr + base);
				VLOG("%s destructors (DT_FINI) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_INIT_ARRAY:
				si.init_array = reinterpret_cast<void**>(dyn->d_un.d_ptr + base);
				VLOG("%s constructors (DT_INIT_ARRAY) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_INIT_ARRAYSZ:
				si.init_array_count = ((unsigned)dyn->d_un.d_val) / sizeof(Elf_Addr);
				VLOG("%s constructors (DT_INIT_ARRAYSZ) %d", si.name, si.init_array_count);
				break;
			case DT_FINI_ARRAY:
				si.fini_array = reinterpret_cast<void**>(dyn->d_un.d_ptr + base);
				VLOG("%s destructors (DT_FINI_ARRAY) found at %x", si.name, dyn->d_un.d_ptr);
				break;
			case DT_FINI_ARRAYSZ:
				si.fini_array_count = ((unsigned)dyn->d_un.d_val) / sizeof(Elf_Addr);
				VLOG("%s destructors (DT_FINI_ARRAYSZ) %d", si.name, si.fini_array_count);
				break;
			case DT_PREINIT_ARRAY:
				si.preinit_array = reinterpret_cast<void**>(dyn->d_un.d_ptr + base);
				VLOG("%s constructors (DT_PREINIT_ARRAY) found at %d", si.name, dyn->d_un.d_ptr);
				break;
			case DT_PREINIT_ARRAYSZ:
				si.preinit_array_count = ((unsigned)dyn->d_un.d_val) / sizeof(Elf_Addr);
				VLOG("%s constructors (DT_PREINIT_ARRAYSZ) %d", si.name,si.preinit_array_count);
				break;
			case DT_TEXTREL:
				si.has_text_relocations = true;
				break;
			case DT_SYMBOLIC:
				si.has_DT_SYMBOLIC = true;
				break;
			case DT_NEEDED:
				++needed_count;
				break;
			case DT_FLAGS:
				if (dyn->d_un.d_val & DF_TEXTREL) {
					si.has_text_relocations = true;
				}
				if (dyn->d_un.d_val & DF_SYMBOLIC) {
					si.has_DT_SYMBOLIC = true;
				}
				break;
			case DT_STRSZ:
				si.strtabsize = dyn->d_un.d_val;
				break;
			case DT_SYMENT:
				si.dynsym_size = dyn->d_un.d_val;
				break;
			case DT_RELENT:
				break;
			case DT_MIPS_RLD_MAP:
				// Set the DT_MIPS_RLD_MAP entry to the address of _r_debug for GDB.
				break;
			case DT_MIPS_RLD_VERSION:
			case DT_MIPS_FLAGS:
			case DT_MIPS_BASE_ADDRESS:
			case DT_MIPS_UNREFEXTNO:
				break;

			case DT_MIPS_SYMTABNO:
				si.mips_symtabno = dyn->d_un.d_val;
				break;

			case DT_MIPS_LOCAL_GOTNO:
				si.mips_local_gotno = dyn->d_un.d_val;
				break;

			case DT_MIPS_GOTSYM:
				si.mips_gotsym = dyn->d_un.d_val;
				break;
			case DT_SONAME:
				si.name = (const char *) (dyn->d_un.d_ptr + base);
				VLOG("soname %s", si.name);
				break;
			default:
				VLOG("Unused DT entry: type 0x%08x arg 0x%08x", dyn->d_tag, dyn->d_un.d_val);
				break;
		}
	}
	DLOG("Dynamic read finish.");
	return true;
}

bool ELFRebuilder::rebuildShdr(){
	shstrtab.clear();
	shdrs.clear();
	Elf_Addr base = si.load_bias;

	Elf_Shdr shdr;
	memset((void*)&shdr, 0, sizeof(shdr));
	shstrtab.push_back('\0');
	shdrs.push_back(shdr);

	// generate .interp
	if(si.interp != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sINTERP = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".interp");
		shstrtab.push_back('\0');
		
		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.interp - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.interp_size;
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 1;
		shdr.sh_entsize = 0;
		
		shdrs.push_back(shdr);
	}

	//generate .dynsym
	if(si.symtab != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sDYNSYM = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".dynsym");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_DYNSYM;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.symtab - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.dynsym_size;
		shdr.sh_link = 0; 		// link to dynstr later
		shdr.sh_info = 1;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 0x10;

		shdrs.push_back(shdr);
	}

	//generate .dynstr
	if(si.strtab != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sDYNSTR = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".dynstr");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_STRTAB;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.strtab - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.strtabsize;
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 1;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .hash
	if(si.hash != 0){
		memset((void*)&shdr, 0, sizeof(shdr));
		sHASH = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".hash");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_HASH;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = si.hash - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = (si.nbucket + si.nchain + 2) * sizeof(Elf_Addr);
		shdr.sh_link = sDYNSYM;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 4;

		shdrs.push_back(shdr);
	}

	//generate .rel.dyn
	if(si.rel != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sRELDYN = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".rel.dyn");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_REL;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.rel - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.rel_count * sizeof(Elf_Rel);
		shdr.sh_link = sDYNSYM;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 8;

		shdrs.push_back(shdr);
	}

	//generate .rel.plt
	if(si.plt_rel != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sRELPLT = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".rel.plt");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_REL;
		shdr.sh_flags = SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.plt_rel - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.plt_rel_count * sizeof(Elf_Rel);
		shdr.sh_link = sDYNSYM;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 8;

		shdrs.push_back(shdr);
	}

	//generate .plt with .rel.plt
	if(si.plt_rel != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sPLT = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".plt");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
		shdr.sh_addr = shdrs[sRELPLT].sh_addr + shdrs[sRELPLT].sh_size;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = 20 + 12 * shdrs[sRELPLT].sh_size/sizeof(Elf_Rel);
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .text&.ARM.extab
	if(si.plt_rel != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sTEXTTAB = shdrs.size();
		Elf_Word sLAST = sTEXTTAB - 1;
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".text&.ARM.extab");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
		shdr.sh_addr = shdrs[sLAST].sh_addr + shdrs[sLAST].sh_size;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = 0;		//TODO: calculate later
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 8;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .ARM.exidx
	if(si.ARM_exidx != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sARMEXIDX = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".ARM.exidx");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_ARM_EXIDX;
		shdr.sh_flags = SHF_ALLOC | SHF_LINK_ORDER;
		shdr.sh_addr = (Elf_Addr)si.ARM_exidx - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.ARM_exidx_count * sizeof(Elf_Addr);
		shdr.sh_link = sTEXTTAB;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 8;

		shdrs.push_back(shdr);
	}

	//generate .fini_array
	if(si.fini_array != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sFINIARRAY = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".fini_array");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_FINI_ARRAY;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.fini_array - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.fini_array_count * sizeof(Elf_Addr);
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .init_array
	if(si.init_array != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sINITARRAY = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".init_array");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_INIT_ARRAY;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.init_array - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.init_array_count * sizeof(Elf_Addr);
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 1;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .dynamic
	if(si.dynamic != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sDYNAMIC = shdrs.size();
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".dynamic");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_DYNAMIC;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = (Elf_Addr)si.dynamic - base;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.dynamic_count * sizeof(Elf_Dyn);
		shdr.sh_link = sDYNSTR;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 8;

		shdrs.push_back(shdr);
	}

	//generate .got
	if(si.plt_got != nullptr){
		memset((void*)&shdr, 0, sizeof(shdr));
		sGOT = shdrs.size();
		Elf_Word sLAST = sGOT - 1;
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".got");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = shdrs[sLAST].sh_addr + shdrs[sLAST].sh_size;
		// In fact the .got is align 8.
		while(shdr.sh_addr & 0x7){ shdr.sh_addr++; }
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = (Elf_Addr)si.plt_got - base + 4*shdrs[sRELPLT].sh_size/sizeof(Elf_Rel) + 12 - shdr.sh_addr;
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .data
	if(true){
		memset((void*)&shdr, 0, sizeof(shdr));
		sDATA = shdrs.size();
		Elf_Word sLAST = sDATA - 1;
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".data");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = shdrs[sLAST].sh_addr + shdrs[sLAST].sh_size;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = si.loadSegEnd - shdr.sh_addr;
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 4;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .bss
	if(true){
		memset((void*)&shdr, 0, sizeof(shdr));
		sBSS = shdrs.size();
		Elf_Word sLAST = sBSS - 1;
		shdr.sh_name = shstrtab.length();
		shstrtab.append(".bss");
		shstrtab.push_back('\0');

		shdr.sh_type = SHT_NOBITS;
		shdr.sh_flags = SHF_WRITE | SHF_ALLOC;
		shdr.sh_addr = shdrs[sLAST].sh_addr + shdrs[sLAST].sh_size;
		shdr.sh_offset = shdr.sh_addr;
		shdr.sh_size = 0;
		shdr.sh_link = 0;
		shdr.sh_info = 0;
		shdr.sh_addralign = 1;
		shdr.sh_entsize = 0;

		shdrs.push_back(shdr);
	}

	//generate .shstrtab
	memset((void*)&shdr, 0, sizeof(shdr));
	sSHSTRTAB = shdrs.size();
	shdr.sh_name = shstrtab.length();
	shstrtab.append(".shstrtab");
	shstrtab.push_back('\0');

	shdr.sh_type = SHT_STRTAB;
	shdr.sh_flags = 0;
	shdr.sh_addr = 0;
	shdr.sh_offset = (Elf_Addr)si.max_load;
	shdr.sh_size = shstrtab.length();
	shdr.sh_link = 0;
	shdr.sh_info = 0;
	shdr.sh_addralign = 1;
	shdr.sh_entsize = 0;

	shdrs.push_back(shdr);

	// patch the link section data
	if(sDYNSYM != 0){
		shdrs[sDYNSYM].sh_link = sDYNSTR;
	}

	// sort shdr by address and recalc size
	for(int i = 1; i < shdrs.size(); i++) {
		for(int j = i + 1; j < shdrs.size(); j++) {
			if(shdrs[i].sh_offset > shdrs[j].sh_offset) {
				// exchange i, j
				Elf_Shdr tmp = shdrs[i];
				shdrs[i] = shdrs[j];
				shdrs[j] = tmp;

				// exchange index
				auto chgIdx = [i, j](Elf_Word &t) {
					if(t == i) {
						t = j;
					} else if(t == j) {
						t = i;
					}
				};
				chgIdx(sDYNSYM);
				chgIdx(sDYNSTR);
				chgIdx(sHASH);
				chgIdx(sRELDYN);
				chgIdx(sRELPLT);
				chgIdx(sPLT);
				chgIdx(sTEXTTAB);
				chgIdx(sARMEXIDX);
				chgIdx(sFINIARRAY);
				chgIdx(sINITARRAY);
				chgIdx(sDYNAMIC);
				chgIdx(sGOT);
				chgIdx(sDATA);
				chgIdx(sBSS);
				chgIdx(sSHSTRTAB);
			}
		}
	}

	if(sTEXTTAB != 0){
		shdrs[sTEXTTAB].sh_size = shdrs[sTEXTTAB + 1].sh_addr - shdrs[sTEXTTAB].sh_addr;
	}

	// recalculate the size of each section 
	for(int i=2;i<shdrs.size();i++){
		if(shdrs[i].sh_offset - shdrs[i-1].sh_offset < shdrs[i-1].sh_size){
			shdrs[i-1].sh_size = shdrs[i].sh_offset - shdrs[i-1].sh_offset;
		}
	}

	VLOG("All sections rebuilded finish.");
	return true;

}


/**
 * Only call if the so file was dumped from memory.
 * We can specify the dump so file using -m option, 
 * and setting the dump base memory with it.
 */
bool ELFRebuilder::rebuildRelocs(){
	if(reader.isDumpSoFile()){
		auto relocate = [](Elf_Addr base, Elf_Rel* rel, size_t count, Elf_Addr dump_base){
			if(rel == nullptr || count == 0) return false;
			for(int i=0;i<count;i++, rel++){
				Elf_Word type = rel->getType();
				Elf_Word sym = rel->getSymbol();

				Elf_Addr* prel = reinterpret_cast<Elf_Addr*>(base + rel->r_offset);
				if(type == 0) continue; //R_*_NONE
				switch(type){
					// Only I know is RELATIVE.
					// It would add a load address when the got table 
					// need to be relocated. 
					// If the so file is dump from memory. The relocate 
					// must have worked. We should subtract the load address.
					case R_386_RELATIVE:
					case R_ARM_RELATIVE:
						*prel = *prel - dump_base;
						break;
					default:
						break;
				}
			}
		};
		relocate(si.load_bias, si.plt_rel, si.plt_rel_count, reader.getDumpSoBase());
		relocate(si.load_bias, si.rel, si.plt_rel_count, reader.getDumpSoBase());
	}
	return true;
}

bool ELFRebuilder::rebuildFinish(){
	size_t load_size = si.max_load - si.min_load;
	rebuild_size = load_size + shstrtab.length() + shdrs.size()*sizeof(Elf_Shdr);
	
	if(rebuild_data != NULL) delete []rebuild_data;
	rebuild_data = new uint8_t[rebuild_size];

	// load segment include elf header
	memcpy(rebuild_data, (void *)si.load_bias, load_size);
	// append shstrtab
	memcpy(rebuild_data + load_size, shstrtab.c_str(), shstrtab.length());
	// append section table
	Elf_Off shdrOffset = load_size + shstrtab.length();
	memcpy(rebuild_data + shdrOffset, (void*)&shdrs[0], shdrs.size()*sizeof(Elf_Shdr));

	// repair the elf header
	elf_header.e_shoff = shdrOffset;
	elf_header.e_shentsize = sizeof(Elf_Shdr);
	elf_header.e_shnum = shdrs.size();
	elf_header.e_shstrndx = sSHSTRTAB;
	memcpy(rebuild_data, &elf_header, sizeof(elf_header));

	VLOG("Rebuild data prepared.");
	return true;
}


/*
 *  This file is part of the SGX-Step enclave execution control framework.
 *
 *  Copyright (C) 2017 Jo Van Bulck <jo.vanbulck@cs.kuleuven.be>,
 *                     Raoul Strackx <raoul.strackx@cs.kuleuven.be>
 *
 *  SGX-Step is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  SGX-Step is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with SGX-Step. If not, see <http://www.gnu.org/licenses/>.
 */

#include "pt.h"
#include "apic.h"
#include "debug.h"
#include <malloc.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

int fd_step = -1, fd_mem = -1;

void __attribute__((constructor)) init_sgx_step( void )
{ 
    ASSERT((fd_step == -1) && (fd_mem == -1)); 
    ASSERT((fd_step = open("/dev/sgx-step", O_RDWR)) >= 0);
    info("/dev/sgx-step opened!");

    ASSERT((fd_mem = open("/dev/mem", O_RDWR|O_SYNC)) >= 0);
    info("/dev/mem opened!");
}

void __attribute__((destructor)) tear_down_sgx_step( void )
{
    /* ensure local APIC timer is restored on process exit */
    if (apic_base)
        apic_timer_deadline();

    close(fd_step);
    close(fd_mem);
}

void *remap(uint64_t phys)
{
    void *map;
    uint64_t virt;
    volatile uint8_t force_mapping;
    ASSERT(fd_mem >= 0);

    map = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
               fd_mem, phys & ~PFN_MASK );
    ASSERT(map != MAP_FAILED);

    virt = ((uint64_t) map) | (phys & PFN_MASK);

    //XXX dereferencing the mapping may cause illegal memory accesses for MMIO
    //regions (eg APIC)
    //force_mapping = *((uint8_t *) virt);

    return (void*) virt;
}

void *remap_page_table_level( void *address, pt_level_t level )
{
	address_mapping_t *map = get_mappings( address );
	void *addr_remapped;
	
	addr_remapped = remap( phys_address( map, level ) );
	free( map );
	
	return addr_remapped;
}

void *remap_private(uint64_t phys)
{
    void *map;
    uint64_t virt;
    volatile uint8_t force_mapping;
    ASSERT(fd_mem >= 0);

    map = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE,
               fd_mem, phys & ~PFN_MASK );
    ASSERT(map != MAP_FAILED);

    virt = ((uint64_t) map) | (phys & PFN_MASK);

    //XXX dereferencing the mapping may cause illegal memory accesses for MMIO
    //regions (eg APIC)
    //force_mapping = *((uint8_t *) virt);

    return (void*) virt;
}

void *remap_page_table_level_private( void *address, pt_level_t level )
{
	address_mapping_t *map = get_mappings( address );
	void *addr_remapped;
	
	addr_remapped = remap_private( phys_address( map, level ) );
	free( map );
	
	return addr_remapped;
}

address_mapping_t *get_mappings( void *address )
{
	address_mapping_t *mapping;
	ASSERT( (mapping = (address_mapping_t *) malloc(sizeof(address_mapping_t))) );
	memset( mapping, 0x00, sizeof( address_mapping_t ) );
	
	mapping->virt = (uint64_t) address;
    ASSERT( ioctl( fd_step, SGX_STEP_IOCTL_GET_PT_MAPPING, mapping ) >= 0);

    return mapping;
}

uint64_t phys_address( address_mapping_t *map, pt_level_t level )
{
	uint64_t base = phys_base_address(map, level);
	uint64_t index = virt_index(map, level);
	
	if ( level == PAGE )
		return base + index;
	else
		return base + index * 64/8;
}

uint64_t phys_base_address( address_mapping_t *map, pt_level_t level )
{
    ASSERT(map);
	
	switch( level )
	{
		case PGD:
			return map->pgd_phys_address;
		case PUD:
		{
			return PGD_PHYS( map->pgd );
		}
		case PMD:
		{
			ASSERT( !PUD_PS( map->pud ) );
		    return PUD_PS_0_PHYS( map->pud );
		}
		case PTE:
		{
			ASSERT( !PUD_PS( map->pud ) && !PMD_PS( map->pmd ) );
			return PMD_PS_0_PHYS( map->pmd );
		}
		case PAGE:
		default:
		{
			if ( PUD_PS( map->pud ) )
				return PUD_PS_1_PHYS( map->pud );
			
			if ( PMD_PS( map->pmd ) )
				return PMD_PS_1_PHYS( map->pmd );
			
			return PT_PHYS( map->pte );
		}
	}
}

uint64_t virt_index( address_mapping_t *map, pt_level_t level )
{
	uint64_t virt;
    ASSERT(map);
	virt = map->virt;
	
	switch( level )
	{
		case PGD:
			return PGD_INDEX( virt );
		case PUD:
			return PUD_INDEX( virt );
		case PMD:
		{
			ASSERT( !PUD_PS( map->pud ));
			return PMD_INDEX( virt );
		}
		case PTE:
		{
			ASSERT( !PUD_PS( map->pud ) && !PMD_PS( map->pmd ) );
			return PT_INDEX( virt );
		}
		case PAGE:
		default:
		{
			if ( PUD_PS( map->pud ) )
				return PAGE1GiB_INDEX( virt );
			
			if ( PMD_PS( map->pmd ) )
				return PAGE2MiB_INDEX( virt );
			
			return PAGE_INDEX( virt );
		}
	}
}

void cpuid( uint64_t *rax, uint64_t *rbx, uint64_t *rcx, uint64_t *rdx )
{
	asm volatile (	"cpuid\n"
			: "=a" (*rax), "=b" (*rbx), "=c" (*rcx), "=d" (*rdx)
			: "a" (*rax), "b" (*rbx), "c" (*rcx), "d" (*rdx) );
}

uint64_t physical_address_width( void )
{
	uint64_t rax, rbx, rcx, rdx;
	static uint64_t width = 0;
	
	//the result is cached to avoid VM exits due to the issuing of cpuid
	if ( width == 0 )
	{
		rax = 0x80000008;
		rbx = 0;
		rcx = 0;
		rdx = 0;

		cpuid( &rax, &rbx, &rcx, &rdx );

		width = ( rax & 0xff );
	}

	return width;
}

void print_page_table( void *address )
{
	address_mapping_t *map;
	
	map = get_mappings( address );
	
	print_mapping( map );
	
	free( map );
}

void print_mapping( address_mapping_t *map )
{
	if ( map == NULL )
		printf( "Mapping [%p]\n", map );
	else if ( PUD_PS( map->pud ) || PMD_PS( map->pmd ) )
		printf( "Mapping [%p] -> maps 2MiB or 1GiB page: please implement\n", map );
	else
	{
		printf( "Mapping [address = %" PRIx64" -> %" PRIx64 " ]\n", map->virt, map->phys );
		printf( "  |-> pgd\n" );
		printf( "       |- base phys:  0x%" PRIx64 "\n", map->pgd_phys_address );
		printf( "       |- index:      %" PRIi64 "\n", virt_index( map, PGD )  );
		printf( "       |- value:      0x%" PRIx64 "\n", map->pgd );
		printf( "       |    |- present:    %" PRIi64 "\n", PRESENT( map->pgd ) );
		printf( "       |    |- accessed:   %" PRIi64 "\n", ACCESSED( map->pgd ) );
		printf( "       |    |- writeable:  %" PRIi64 "\n", WRITABLE( map->pgd ) );
		printf( "       |    |- executable: %" PRIi64 "\n", EXECUTABLE( map->pgd ) );
		printf( "       |\n" );
		printf( "       |-> pud\n" );
		printf( "            |- base phys:  0x%" PRIx64 "\n", PGD_PHYS( map->pgd ) );
		printf( "            |- index:      %" PRIi64 "\n", virt_index( map, PUD )  );
		printf( "            |- value:      0x%" PRIx64 "\n", map->pud );
		printf( "            |    |- present:    %" PRIi64 "\n", PRESENT( map->pud ) );
		printf( "            |    |- page size:  %" PRIi64 "\n", PUD_PS( map->pud ) );
		printf( "            |    |- accessed:   %" PRIi64 "\n", ACCESSED( map->pud ) );
		printf( "            |    |- writeable:  %" PRIi64 "\n", WRITABLE( map->pud ) );
		printf( "            |    |- executable: %" PRIi64 "\n", EXECUTABLE( map->pud ) );
		printf( "            |\n" );
		printf( "            |-> pmd\n" );
		printf( "                 |- base phys:  0x%" PRIx64 "\n", PUD_PS_0_PHYS( map->pud ) );
		printf( "                 |- index:      %" PRIi64 "\n", virt_index( map, PMD )  );
		printf( "                 |- value:      0x%" PRIx64 "\n", map->pmd );
		printf( "                 |    |- present:    %" PRIi64 "\n", PRESENT( map->pmd ) );
		printf( "                 |    |- page size:  %" PRIi64 "\n", PMD_PS( map->pud ) );
		printf( "                 |    |- accessed:   %" PRIi64 "\n", ACCESSED( map->pmd ) );
		printf( "                 |    |- writable:   %" PRIi64 "\n", WRITABLE( map->pmd ) );
		printf( "                 |    |- executable: %" PRIi64 "\n", EXECUTABLE( map->pmd ) );
		printf( "                 |\n" );
		printf( "                 |-> pte\n" );
		printf( "                     |- base phys:  0x%" PRIx64 "\n", PMD_PS_0_PHYS( map->pmd ) );
		printf( "                     |- index:      %" PRIi64 "\n", virt_index( map, PTE )  );
		printf( "                     |- value:      0x%" PRIx64 "\n", map->pte );
		printf( "                     |    |- present:    %" PRIi64 "\n", PRESENT( map->pte ) );
		printf( "                     |    |- accessed:   %" PRIi64 "\n", ACCESSED( map->pte ) );
		printf( "                     |    |- writable:   %" PRIi64 "\n", WRITABLE( map->pte ) );
		printf( "                     |    |- executable: %" PRIi64 "\n", EXECUTABLE( map->pte ) );
		printf( "                     |    |- dirty:      %" PRIi64 "\n", DIRTY( map->pte ) );
		printf( "                     |\n" );
		printf( "                     |-> PAGE\n" );
		printf( "                           |- virt address:      0x%" PRIx64 "\n", map->virt );
		printf( "                           |- index:             0x%" PRIx64 "\n", virt_index( map, PAGE )  );
		printf( "                           |- base phys address: 0x%" PRIx64 "\n", PT_PHYS( map->pte ) );
		printf( "                           |- phys address:      0x%" PRIx64 "\n", PT_PHYS( map->pte ) + virt_index( map, PAGE )  );
	}
}

/* ARC-specific support for 32-bit ELF
   Copyright (C) 1994-2016 Free Software Foundation, Inc.
   Contributed by Cupertino Miranda (cmiranda@synopsys.com).

   This file is part of BFD, the Binary File Descriptor library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */

#include "sysdep.h"
#include "bfd.h"
#include "libbfd.h"
#include "elf-bfd.h"
#include "elf/arc.h"
#include "libiberty.h"
#include "opcode/arc-func.h"
#include "arc-plt.h"

#ifdef DEBUG
# define PR_DEBUG(fmt, args...) fprintf (stderr, fmt, ##args)
#else
# define PR_DEBUG(fmt, args...)
#endif

/* #define ARC_ENABLE_DEBUG 1 */
#ifndef ARC_ENABLE_DEBUG
#define ARC_DEBUG(...)
#else
static char *
name_for_global_symbol (struct elf_link_hash_entry *h)
{
  static char *local_str = "(local)";
  if (h == NULL)
    return local_str;
  else
    return h->root.root.string;
}
#define ARC_DEBUG(args...) fprintf (stderr, ##args)
#endif


#define ADD_RELA(BFD, SECTION, OFFSET, SYM_IDX, TYPE, ADDEND)		\
  {									\
    struct elf_link_hash_table *_htab = elf_hash_table (info);		\
    Elf_Internal_Rela _rel;						\
    bfd_byte * _loc;							\
									\
    _loc = _htab->srel##SECTION->contents				\
      + ((_htab->srel##SECTION->reloc_count)				\
	 * sizeof (Elf32_External_Rela));				\
    _htab->srel##SECTION->reloc_count++;				\
    _rel.r_addend = ADDEND;						\
    _rel.r_offset = (_htab->s##SECTION)->output_section->vma		\
      + (_htab->s##SECTION)->output_offset + OFFSET;			\
    BFD_ASSERT ((long) SYM_IDX != -1);					\
    _rel.r_info = ELF32_R_INFO (SYM_IDX, TYPE);				\
    bfd_elf32_swap_reloca_out (BFD, &_rel, _loc);			\
  }

struct arc_local_data
{
  bfd_vma	  sdata_begin_symbol_vma;
  asection *      sdata_output_section;
  bfd_vma	  got_symbol_vma;
};

struct arc_local_data global_arc_data =
{
  .sdata_begin_symbol_vma = 0,
  .sdata_output_section = NULL,
  .got_symbol_vma = 0,
};

struct dynamic_sections
{
  bfd_boolean	  initialized;
  asection *      sgot;
  asection *      srelgot;
  asection *      sgotplt;
  asection *      srelgotplt;
  asection *      sdyn;
  asection *      splt;
  asection *      srelplt;
};

enum dyn_section_types
{
  got = 0,
  relgot,
  gotplt,
  dyn,
  plt,
  relplt,
  DYN_SECTION_TYPES_END
};

const char * dyn_section_names[DYN_SECTION_TYPES_END] =
{
  ".got",
  ".rela.got",
  ".got.plt",
  ".dynamic",
  ".plt",
  ".rela.plt"
};

enum tls_type_e
{
  GOT_UNKNOWN = 0,
  GOT_NORMAL,
  GOT_TLS_GD,
  GOT_TLS_IE,
  GOT_TLS_LE
};

enum tls_got_entries
{
  TLS_GOT_NONE = 0,
  TLS_GOT_MOD,
  TLS_GOT_OFF,
  TLS_GOT_MOD_AND_OFF
};

struct got_entry
{
  struct got_entry *next;
  enum tls_type_e type;
  bfd_vma offset;
  bfd_boolean processed;
  bfd_boolean created_dyn_relocation;
  enum tls_got_entries existing_entries;
};

static void
new_got_entry_to_list (struct got_entry **list,
		       enum tls_type_e type,
		       bfd_vma offset,
		       enum tls_got_entries existing_entries)
{
  /* Find list end.  Avoid having multiple entries of the same
     type.  */
  struct got_entry **p = list;
  while (*p != NULL)
    {
      if ((*p)->type == type)
	return;
      p = &((*p)->next);
    }

  struct got_entry *entry =
		      (struct got_entry *) malloc (sizeof(struct got_entry));

  entry->type = type;
  entry->offset = offset;
  entry->next = NULL;
  entry->processed = FALSE;
  entry->created_dyn_relocation = FALSE;
  entry->existing_entries = existing_entries;

  /* Add the entry to the end of the list.  */
  *p = entry;
}

static bfd_boolean
symbol_has_entry_of_type (struct got_entry *list, enum tls_type_e type)
{
  while (list != NULL)
    {
      if (list->type == type)
	return TRUE;
      list = list->next;
    }

  return FALSE;
}

/* The default symbols representing the init and fini dyn values.
   TODO: Check what is the relation of those strings with arclinux.em
   and DT_INIT.  */
#define INIT_SYM_STRING "_init"
#define FINI_SYM_STRING "_fini"

char * init_str = INIT_SYM_STRING;
char * fini_str = FINI_SYM_STRING;

#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
      case VALUE: \
	return "R_" #TYPE; \
	break;

static ATTRIBUTE_UNUSED const char *
reloc_type_to_name (unsigned int type)
{
  switch (type)
    {
      #include "elf/arc-reloc.def"

      default:
	return "UNKNOWN";
	break;
    }
}
#undef ARC_RELOC_HOWTO

/* Try to minimize the amount of space occupied by relocation tables
   on the ROM (not that the ROM won't be swamped by other ELF overhead).  */

#define USE_REL 1

static ATTRIBUTE_UNUSED bfd_boolean
is_reloc_PC_relative (reloc_howto_type *howto)
{
  return (strstr (howto->name, "PC") != NULL) ? TRUE : FALSE;
}

static bfd_boolean
is_reloc_SDA_relative (reloc_howto_type *howto)
{
  return (strstr (howto->name, "SDA") != NULL) ? TRUE : FALSE;
}

static bfd_boolean
is_reloc_for_GOT (reloc_howto_type * howto)
{
  if (strstr (howto->name, "TLS") != NULL)
    return FALSE;
  return (strstr (howto->name, "GOT") != NULL) ? TRUE : FALSE;
}

static bfd_boolean
is_reloc_for_PLT (reloc_howto_type * howto)
{
  return (strstr (howto->name, "PLT") != NULL) ? TRUE : FALSE;
}

static bfd_boolean
is_reloc_for_TLS (reloc_howto_type *howto)
{
  return (strstr (howto->name, "TLS") != NULL) ? TRUE : FALSE;
}

#define arc_bfd_get_8(A,B,C) bfd_get_8(A,B)
#define arc_bfd_get_16(A,B,C) bfd_get_16(A,B)
#define arc_bfd_get_32(A,B,C) bfd_get_32(A,B)
#define arc_bfd_put_8(A,B,C,D) bfd_put_8(A,B,C)
#define arc_bfd_put_16(A,B,C,D) bfd_put_16(A,B,C)
#define arc_bfd_put_32(A,B,C,D) bfd_put_32(A,B,C)


static bfd_reloc_status_type
arc_elf_reloc (bfd *abfd ATTRIBUTE_UNUSED,
	       arelent *reloc_entry,
	       asymbol *symbol_in,
	       void *data ATTRIBUTE_UNUSED,
	       asection *input_section,
	       bfd *output_bfd,
	       char ** error_message ATTRIBUTE_UNUSED)
{
  if (output_bfd != NULL)
    {
      reloc_entry->address += input_section->output_offset;

      /* In case of relocateable link and if the reloc is against a
	 section symbol, the addend needs to be adjusted according to
	 where the section symbol winds up in the output section.  */
      if ((symbol_in->flags & BSF_SECTION_SYM) && symbol_in->section)
	reloc_entry->addend += symbol_in->section->output_offset;

      return bfd_reloc_ok;
    }

  return bfd_reloc_continue;
}


#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  TYPE = VALUE,
enum howto_list
{
#include "elf/arc-reloc.def"
  HOWTO_LIST_LAST
};
#undef ARC_RELOC_HOWTO

#define ARC_RELOC_HOWTO(TYPE, VALUE, RSIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  [TYPE] = HOWTO (R_##TYPE, 0, RSIZE, BITSIZE, FALSE, 0, complain_overflow_##OVERFLOW, arc_elf_reloc, "R_" #TYPE, FALSE, 0, 0, FALSE),

static struct reloc_howto_struct elf_arc_howto_table[] =
{
#include "elf/arc-reloc.def"
/* Example of what is generated by the preprocessor.  Currently kept as an
   example.
 HOWTO (R_ARC_NONE, // Type.
    0, // Rightshift.
    2, // Size (0 = byte, 1 = short, 2 = long).
    32, // Bitsize.
    FALSE, // PC_relative.
    0, // Bitpos.
    complain_overflow_bitfield, // Complain_on_overflow.
    bfd_elf_generic_reloc, // Special_function.
    "R_ARC_NONE", // Name.
    TRUE, // Partial_inplace.
    0, // Src_mask.
    0, // Dst_mask.
    FALSE), // PCrel_offset.
*/
};
#undef ARC_RELOC_HOWTO

static void arc_elf_howto_init (void)
{
#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  elf_arc_howto_table[TYPE].pc_relative = \
    (strstr (#FORMULA, " P ") != NULL || strstr (#FORMULA, " PDATA ") != NULL); \
  elf_arc_howto_table[TYPE].dst_mask = RELOC_FUNCTION(0, ~0); \
  /* Only 32 bit data relocations should be marked as ME.  */ \
  if (strstr (#FORMULA, " ME ") != NULL) \
    { \
      BFD_ASSERT (SIZE == 2); \
    }

#include "elf/arc-reloc.def"

}
#undef ARC_RELOC_HOWTO


#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  [TYPE] = VALUE,
const int howto_table_lookup[] =
{
#include "elf/arc-reloc.def"
};
#undef ARC_RELOC_HOWTO

static reloc_howto_type *
arc_elf_howto (unsigned int r_type)
{
  if (elf_arc_howto_table[R_ARC_32].dst_mask == 0)
    arc_elf_howto_init ();
  return &elf_arc_howto_table[r_type];
}

/* Map BFD reloc types to ARC ELF reloc types.  */

struct arc_reloc_map
{
  bfd_reloc_code_real_type  bfd_reloc_val;
  unsigned char             elf_reloc_val;
};

#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  { BFD_RELOC_##TYPE, R_##TYPE },
static const struct arc_reloc_map arc_reloc_map[] =
{
#include "elf/arc-reloc.def"

  {BFD_RELOC_NONE,  R_ARC_NONE},
  {BFD_RELOC_8,  R_ARC_8},
  {BFD_RELOC_16, R_ARC_16},
  {BFD_RELOC_24, R_ARC_24},
  {BFD_RELOC_32, R_ARC_32},
};
#undef ARC_RELOC_HOWTO

typedef ATTRIBUTE_UNUSED bfd_vma (*replace_func) (unsigned, int ATTRIBUTE_UNUSED);

#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  case TYPE: \
    func = (void *) RELOC_FUNCTION; \
    break;
static replace_func
get_replace_function (bfd *abfd, unsigned int r_type)
{
  void *func = NULL;

  switch (r_type)
    {
      #include "elf/arc-reloc.def"
    }

  if (func == replace_bits24 && bfd_big_endian (abfd))
    return (replace_func) replace_bits24_be;

  return (replace_func) func;
}
#undef ARC_RELOC_HOWTO

static reloc_howto_type *
arc_elf32_bfd_reloc_type_lookup (bfd * abfd ATTRIBUTE_UNUSED,
				 bfd_reloc_code_real_type code)
{
  unsigned int i;

  for (i = ARRAY_SIZE (arc_reloc_map); i--;)
    {
      if (arc_reloc_map[i].bfd_reloc_val == code)
	return arc_elf_howto (arc_reloc_map[i].elf_reloc_val);
    }

  return NULL;
}

/* Function to set the ELF flag bits.  */
static bfd_boolean
arc_elf_set_private_flags (bfd *abfd, flagword flags)
{
  elf_elfheader (abfd)->e_flags = flags;
  elf_flags_init (abfd) = TRUE;
  return TRUE;
}

/* Print private flags.  */
static bfd_boolean
arc_elf_print_private_bfd_data (bfd *abfd, void * ptr)
{
  FILE *file = (FILE *) ptr;
  flagword flags;

  BFD_ASSERT (abfd != NULL && ptr != NULL);

  /* Print normal ELF private data.  */
  _bfd_elf_print_private_bfd_data (abfd, ptr);

  flags = elf_elfheader (abfd)->e_flags;
  fprintf (file, _("private flags = 0x%lx:"), (unsigned long) flags);

  switch (flags & EF_ARC_MACH_MSK)
    {
    case EF_ARC_CPU_ARCV2HS : fprintf (file, " -mcpu=ARCv2HS");    break;
    case EF_ARC_CPU_ARCV2EM : fprintf (file, " -mcpu=ARCv2EM");    break;
    case E_ARC_MACH_ARC600  : fprintf (file, " -mcpu=ARC600");     break;
    case E_ARC_MACH_ARC601  : fprintf (file, " -mcpu=ARC601");     break;
    case E_ARC_MACH_ARC700  : fprintf (file, " -mcpu=ARC700");     break;
    default:
      fprintf (file, "-mcpu=unknown");
      break;
    }

  switch (flags & EF_ARC_OSABI_MSK)
    {
    case E_ARC_OSABI_ORIG : fprintf (file, " (ABI:legacy)"); break;
    case E_ARC_OSABI_V2   : fprintf (file, " (ABI:v2)");     break;
    case E_ARC_OSABI_V3   : fprintf (file, " (ABI:v3)");     break;
    default:
      fprintf (file, "(ABI:unknown)");
      break;
    }

  fputc ('\n', file);
  return TRUE;
}

/* Copy backend specific data from one object module to another.  */

static bfd_boolean
arc_elf_copy_private_bfd_data (bfd *ibfd, bfd *obfd)
{
  if (bfd_get_flavour (ibfd) != bfd_target_elf_flavour
      || bfd_get_flavour (obfd) != bfd_target_elf_flavour)
    return TRUE;

  BFD_ASSERT (!elf_flags_init (obfd)
	      || elf_elfheader (obfd)->e_flags == elf_elfheader (ibfd)->e_flags);

  elf_elfheader (obfd)->e_flags = elf_elfheader (ibfd)->e_flags;
  elf_flags_init (obfd) = TRUE;

  /* Copy object attributes.  */
  _bfd_elf_copy_obj_attributes (ibfd, obfd);

  return _bfd_elf_copy_private_bfd_data (ibfd, obfd);
}

static reloc_howto_type *
bfd_elf32_bfd_reloc_name_lookup (bfd * abfd ATTRIBUTE_UNUSED,
				 const char *r_name)
{
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE (elf_arc_howto_table); i++)
    if (elf_arc_howto_table[i].name != NULL
	&& strcasecmp (elf_arc_howto_table[i].name, r_name) == 0)
      return arc_elf_howto (i);

  return NULL;
}

/* Set the howto pointer for an ARC ELF reloc.  */

static void
arc_info_to_howto_rel (bfd * abfd ATTRIBUTE_UNUSED,
		       arelent * cache_ptr,
		       Elf_Internal_Rela * dst)
{
  unsigned int r_type;

  r_type = ELF32_R_TYPE (dst->r_info);
  BFD_ASSERT (r_type < (unsigned int) R_ARC_max);
  cache_ptr->howto = arc_elf_howto (r_type);
}

/* Merge backend specific data from an object file to the output
   object file when linking.  */

static bfd_boolean
arc_elf_merge_private_bfd_data (bfd *ibfd, bfd *obfd)
{
  unsigned short mach_ibfd;
  static unsigned short mach_obfd = EM_NONE;
  flagword out_flags;
  flagword in_flags;
  asection *sec;

   /* Check if we have the same endianess.  */
  if (! _bfd_generic_verify_endian_match (ibfd, obfd))
    {
      _bfd_error_handler (_("ERROR: Endian Match failed. Attempting to link "
			    "%B with binary %s of opposite endian-ness"),
			  ibfd, bfd_get_filename (obfd));
      return FALSE;
    }

  /* Collect ELF flags.  */
  in_flags = elf_elfheader (ibfd)->e_flags & EF_ARC_MACH_MSK;
  out_flags = elf_elfheader (obfd)->e_flags & EF_ARC_MACH_MSK;

  if (!elf_flags_init (obfd)) /* First call, no flags set.  */
    {
      elf_flags_init (obfd) = TRUE;
      out_flags = in_flags;
    }

  if (bfd_get_flavour (ibfd) != bfd_target_elf_flavour
      || bfd_get_flavour (obfd) != bfd_target_elf_flavour)
    return TRUE;

  /* Check to see if the input BFD actually contains any sections.  Do
     not short-circuit dynamic objects; their section list may be
     emptied by elf_link_add_object_symbols.  */
  if (!(ibfd->flags & DYNAMIC))
    {
      bfd_boolean null_input_bfd = TRUE;
      bfd_boolean only_data_sections = TRUE;

      for (sec = ibfd->sections; sec != NULL; sec = sec->next)
	{
	  if ((bfd_get_section_flags (ibfd, sec)
	       & (SEC_LOAD | SEC_CODE | SEC_HAS_CONTENTS))
	      == (SEC_LOAD | SEC_CODE | SEC_HAS_CONTENTS))
	    only_data_sections = FALSE;

	  null_input_bfd = FALSE;
	}

      if (null_input_bfd || only_data_sections)
	return TRUE;
    }

  /* Complain about various flag/architecture mismatches.  */
  mach_ibfd = elf_elfheader (ibfd)->e_machine;
  if (mach_obfd == EM_NONE)
    {
      mach_obfd = mach_ibfd;
    }
  else
    {
      if (mach_ibfd != mach_obfd)
	{
	  _bfd_error_handler (_("ERROR: Attempting to link %B "
				"with a binary %s of different architecture"),
			      ibfd, bfd_get_filename (obfd));
	  return FALSE;
	}
      else if (in_flags != out_flags)
	{
	  /* Warn if different flags.  */
	  (*_bfd_error_handler)
	    (_("%s: uses different e_flags (0x%lx) fields than "
	       "previous modules (0x%lx)"),
	     bfd_get_filename (ibfd), (long)in_flags, (long)out_flags);
	  if (in_flags && out_flags)
	    return FALSE;
	  /* MWDT doesnt set the eflags hence make sure we choose the
	     eflags set by gcc.  */
	  in_flags = in_flags > out_flags ? in_flags : out_flags;
	}
    }

  /* Update the flags.  */
  elf_elfheader (obfd)->e_flags = in_flags;

  if (bfd_get_mach (obfd) < bfd_get_mach (ibfd))
    {
      return bfd_set_arch_mach (obfd, bfd_arch_arc, bfd_get_mach (ibfd));
    }

  return TRUE;
}

/* Set the right machine number for an ARC ELF file.  */
static bfd_boolean
arc_elf_object_p (bfd * abfd)
{
  /* Make sure this is initialised, or you'll have the potential of passing
     garbage---or misleading values---into the call to
     bfd_default_set_arch_mach ().  */
  int		  mach = bfd_mach_arc_arc700;
  unsigned long   arch = elf_elfheader (abfd)->e_flags & EF_ARC_MACH_MSK;
  unsigned	  e_machine = elf_elfheader (abfd)->e_machine;

  if (e_machine == EM_ARC_COMPACT || e_machine == EM_ARC_COMPACT2)
    {
      switch (arch)
	{
	  case E_ARC_MACH_ARC600:
	    mach = bfd_mach_arc_arc600;
	    break;
	  case E_ARC_MACH_ARC601:
	    mach = bfd_mach_arc_arc601;
	    break;
	  case E_ARC_MACH_ARC700:
	    mach = bfd_mach_arc_arc700;
	    break;
	  case E_ARC_MACH_NPS400:
	    mach = bfd_mach_arc_nps400;
	    break;
	  case EF_ARC_CPU_ARCV2HS:
	  case EF_ARC_CPU_ARCV2EM:
	    mach = bfd_mach_arc_arcv2;
	    break;
	  default:
	    mach = (e_machine == EM_ARC_COMPACT) ?
	      bfd_mach_arc_arc700 : bfd_mach_arc_arcv2;
	    break;
	}
    }
  else
    {
      if (e_machine == EM_ARC)
	{
	  (*_bfd_error_handler)
	    (_("Error: The ARC4 architecture is no longer supported.\n"));
	  return FALSE;
	}
      else
	{
	  (*_bfd_error_handler)
	    (_("Warning: unset or old architecture flags. \n"
	       "	       Use default machine.\n"));
	}
    }

  return bfd_default_set_arch_mach (abfd, bfd_arch_arc, mach);
}

/* The final processing done just before writing out an ARC ELF object file.
   This gets the ARC architecture right based on the machine number.  */

static void
arc_elf_final_write_processing (bfd * abfd,
				bfd_boolean linker ATTRIBUTE_UNUSED)
{
  unsigned long emf;

  switch (bfd_get_mach (abfd))
    {
    case bfd_mach_arc_arc600:
      emf = EM_ARC_COMPACT;
      break;
    case bfd_mach_arc_arc601:
      emf = EM_ARC_COMPACT;
      break;
    case bfd_mach_arc_arc700:
      emf = EM_ARC_COMPACT;
      break;
    case bfd_mach_arc_nps400:
      emf = EM_ARC_COMPACT;
      break;
    case bfd_mach_arc_arcv2:
      emf = EM_ARC_COMPACT2;
      break;
    default:
      abort ();
    }

  elf_elfheader (abfd)->e_machine = emf;

  /* Record whatever is the current syscall ABI version.  */
  elf_elfheader (abfd)->e_flags |= E_ARC_OSABI_CURRENT;
}

#define BFD_DEBUG_PIC(...)

struct arc_relocation_data
{
  bfd_signed_vma  reloc_offset;
  bfd_signed_vma  reloc_addend;
  bfd_signed_vma  got_offset_value;

  bfd_signed_vma  sym_value;
  asection *	  sym_section;

  reloc_howto_type *howto;

  asection *	  input_section;

  bfd_signed_vma  sdata_begin_symbol_vma;
  bfd_boolean	  sdata_begin_symbol_vma_set;
  bfd_signed_vma  got_symbol_vma;

  bfd_boolean	  should_relocate;
};

static void
debug_arc_reloc (struct arc_relocation_data reloc_data)
{
  PR_DEBUG ("Reloc type=%s, should_relocate = %s\n",
	   reloc_data.howto->name,
	   reloc_data.should_relocate ? "true" : "false");
  PR_DEBUG ("  offset = 0x%x, addend = 0x%x\n",
	   (unsigned int) reloc_data.reloc_offset,
	   (unsigned int) reloc_data.reloc_addend);
  PR_DEBUG (" Symbol:\n");
  PR_DEBUG ("  value = 0x%08x\n",
	   (unsigned int) reloc_data.sym_value);
  if (reloc_data.sym_section != NULL)
    {
      PR_DEBUG ("IN IF\n");
      PR_DEBUG (
	       "  section name = %s, output_offset 0x%08x",
	       reloc_data.sym_section->name,
	       (unsigned int) reloc_data.sym_section->output_offset);
      if (reloc_data.sym_section->output_section != NULL)
	{
	  PR_DEBUG (
		   ", output_section->vma = 0x%08x",
		   ((unsigned int) reloc_data.sym_section->output_section->vma));
	}

      PR_DEBUG ( "\n");
    }
  else
    {
      PR_DEBUG ( "  symbol section is NULL\n");
    }

  PR_DEBUG ( " Input_section:\n");
  if (reloc_data.input_section != NULL)
    {
      PR_DEBUG (
	       "  section name = %s, output_offset 0x%08x, output_section->vma = 0x%08x\n",
	       reloc_data.input_section->name,
	       (unsigned int) reloc_data.input_section->output_offset,
	       (unsigned int) reloc_data.input_section->output_section->vma);
      PR_DEBUG ( "  changed_address = 0x%08x\n",
	       (unsigned int) (reloc_data.input_section->output_section->vma +
	       reloc_data.input_section->output_offset +
	       reloc_data.reloc_offset));
    }
  else
    {
      PR_DEBUG ( "	input section is NULL\n");
    }
}

static bfd_vma
middle_endian_convert (bfd_vma insn, bfd_boolean do_it)
{
  if (do_it)
    {
      insn =
	((insn & 0xffff0000) >> 16) |
	((insn & 0xffff) << 16);
    }
  return insn;
}

#define ME(reloc) (reloc)

#define IS_ME(FORMULA,BFD) ((strstr (FORMULA, "ME") != NULL) \
			    && (!bfd_big_endian (BFD)))

#define S ((bfd_signed_vma) (reloc_data.sym_value			\
	   + (reloc_data.sym_section->output_section != NULL ?		\
	      (reloc_data.sym_section->output_offset			\
	       + reloc_data.sym_section->output_section->vma) : 0)))
#define L ((bfd_signed_vma) (reloc_data.sym_value			\
	   + (reloc_data.sym_section->output_section != NULL ?		\
	      (reloc_data.sym_section->output_offset			\
	      + reloc_data.sym_section->output_section->vma) : 0)))
#define A (reloc_data.reloc_addend)
#define B (0)
#define G (reloc_data.got_offset_value)
#define GOT (reloc_data.got_symbol_vma)
#define GOT_BEGIN (htab->sgot->output_section->vma)

#define MES (0)
	/* P: relative offset to PCL The offset should be to the
	  current location aligned to 32 bits.  */
#define P ((bfd_signed_vma) (						\
	   (								\
	    (reloc_data.input_section->output_section != NULL ?		\
	     reloc_data.input_section->output_section->vma : 0)		\
	    + reloc_data.input_section->output_offset			\
	    + (reloc_data.reloc_offset - (bitsize >= 32 ? 4 : 0)))	\
	   & ~0x3))
#define PDATA ((bfd_signed_vma) ( \
	    (reloc_data.input_section->output_section->vma \
	     + reloc_data.input_section->output_offset \
	     + (reloc_data.reloc_offset))))
#define SECTSTART (bfd_signed_vma) (reloc_data.input_section->output_offset)
#define _SDA_BASE_ (bfd_signed_vma) (reloc_data.sdata_begin_symbol_vma)
#define TLS_REL (bfd_signed_vma) \
  ((elf_hash_table (info))->tls_sec->output_section->vma)
#define TLS_TBSS (8)
#define TCB_SIZE (8)

#define none (0)

#define PRINT_DEBUG_RELOC_INFO_BEFORE(FORMULA, TYPE) \
    {\
      asection *sym_section = reloc_data.sym_section; \
      asection *input_section = reloc_data.input_section; \
      ARC_DEBUG ("RELOC_TYPE = " TYPE "\n"); \
      ARC_DEBUG ("FORMULA = " FORMULA "\n"); \
      ARC_DEBUG ("S = 0x%x\n", S); \
      ARC_DEBUG ("A = 0x%x\n", A); \
      ARC_DEBUG ("L = 0x%x\n", L); \
      if (sym_section->output_section != NULL) \
	{ \
	  ARC_DEBUG ("symbol_section->vma = 0x%x\n", \
	     sym_section->output_section->vma + sym_section->output_offset); \
	} \
      else \
	{ \
	  ARC_DEBUG ("symbol_section->vma = NULL\n"); \
	} \
      if (input_section->output_section != NULL) \
	{ \
	  ARC_DEBUG ("symbol_section->vma = 0x%x\n", \
	     input_section->output_section->vma + input_section->output_offset); \
	} \
      else \
	{ \
	  ARC_DEBUG ("symbol_section->vma = NULL\n"); \
	} \
      ARC_DEBUG ("PCL = 0x%x\n", P); \
      ARC_DEBUG ("P = 0x%x\n", P); \
      ARC_DEBUG ("G = 0x%x\n", G); \
      ARC_DEBUG ("SDA_OFFSET = 0x%x\n", _SDA_BASE_); \
      ARC_DEBUG ("SDA_SET = %d\n", reloc_data.sdata_begin_symbol_vma_set); \
      ARC_DEBUG ("GOT_OFFSET = 0x%x\n", GOT); \
      ARC_DEBUG ("relocation = 0x%08x\n", relocation); \
      ARC_DEBUG ("before = 0x%08x\n", (unsigned int) insn); \
      ARC_DEBUG ("data   = 0x%08x (%u) (%d)\n", (unsigned int) relocation, (unsigned int) relocation, (int) relocation); \
    }

#define PRINT_DEBUG_RELOC_INFO_AFTER \
    { \
      ARC_DEBUG ("after  = 0x%08x\n", (unsigned int) insn); \
    }

#define ARC_RELOC_HOWTO(TYPE, VALUE, SIZE, BITSIZE, RELOC_FUNCTION, OVERFLOW, FORMULA) \
  case R_##TYPE: \
    { \
      bfd_signed_vma bitsize ATTRIBUTE_UNUSED = BITSIZE; \
      relocation = FORMULA  ; \
      PRINT_DEBUG_RELOC_INFO_BEFORE (#FORMULA, #TYPE); \
      insn = middle_endian_convert (insn, IS_ME (#FORMULA, abfd)); \
      insn = (* get_replace_function (abfd, TYPE)) (insn, relocation); \
      insn = middle_endian_convert (insn, IS_ME (#FORMULA, abfd)); \
      PRINT_DEBUG_RELOC_INFO_AFTER \
    } \
    break;

static bfd_reloc_status_type
arc_do_relocation (bfd_byte * contents,
		   struct arc_relocation_data reloc_data,
		   struct bfd_link_info *info)
{
  bfd_signed_vma relocation = 0;
  bfd_vma insn;
  bfd_vma orig_insn ATTRIBUTE_UNUSED;
  bfd * abfd = reloc_data.input_section->owner;
  struct elf_link_hash_table *htab ATTRIBUTE_UNUSED = elf_hash_table (info);

  if (reloc_data.should_relocate == FALSE)
    return bfd_reloc_ok;

  switch (reloc_data.howto->size)
    {
      case 2:
	insn = arc_bfd_get_32 (abfd,
			       contents + reloc_data.reloc_offset,
			       reloc_data.input_section);
	break;
      case 1:
	insn = arc_bfd_get_16 (abfd,
			       contents + reloc_data.reloc_offset,
			       reloc_data.input_section);
	break;
      case 0:
	insn = arc_bfd_get_8 (abfd,
			       contents + reloc_data.reloc_offset,
			       reloc_data.input_section);
	break;
      default:
	insn = 0;
	BFD_ASSERT (0);
	break;
    }

  orig_insn = insn;

  switch (reloc_data.howto->type)
    {
#include "elf/arc-reloc.def"

      default:
	BFD_ASSERT (0);
	break;
    }

  /* Check for relocation overflow.  */
  if (reloc_data.howto->complain_on_overflow != complain_overflow_dont)
    {
      bfd_reloc_status_type flag;
      flag = bfd_check_overflow (reloc_data.howto->complain_on_overflow,
				 reloc_data.howto->bitsize,
				 reloc_data.howto->rightshift,
				 bfd_arch_bits_per_address (abfd),
				 relocation);

#undef  DEBUG_ARC_RELOC
#define DEBUG_ARC_RELOC(A) debug_arc_reloc (A)
      if (flag != bfd_reloc_ok)
	{
	  PR_DEBUG ( "Relocation overflows !!!!\n");

	  DEBUG_ARC_RELOC (reloc_data);

	  PR_DEBUG (
		  "Relocation value = signed -> %d, unsigned -> %u"
		  ", hex -> (0x%08x)\n",
		  (int) relocation,
		  (unsigned int) relocation,
		  (unsigned int) relocation);
	  return flag;
	}
    }
#undef  DEBUG_ARC_RELOC
#define DEBUG_ARC_RELOC(A)

  switch (reloc_data.howto->size)
    {
      case 2:
	arc_bfd_put_32 (abfd, insn,
		       contents + reloc_data.reloc_offset,
		       reloc_data.input_section);
	break;
      case 1:
	arc_bfd_put_16 (abfd, insn,
		       contents + reloc_data.reloc_offset,
		       reloc_data.input_section);
	break;
      case 0:
	arc_bfd_put_8 (abfd, insn,
		       contents + reloc_data.reloc_offset,
		       reloc_data.input_section);
	break;
      default:
	ARC_DEBUG ("size = %d\n", reloc_data.howto->size);
	BFD_ASSERT (0);
	break;
    }

  return bfd_reloc_ok;
}
#undef S
#undef A
#undef B
#undef G
#undef GOT
#undef L
#undef MES
#undef P
#undef SECTSTAR
#undef SECTSTART
#undef _SDA_BASE_
#undef none

#undef ARC_RELOC_HOWTO

static struct got_entry **
arc_get_local_got_ents (bfd * abfd)
{
  static struct got_entry **local_got_ents = NULL;

  if (local_got_ents == NULL)
    {
      size_t	   size;
      Elf_Internal_Shdr *symtab_hdr = &((elf_tdata (abfd))->symtab_hdr);

      size = symtab_hdr->sh_info * sizeof (bfd_vma);
      local_got_ents = (struct got_entry **)
	bfd_alloc (abfd, sizeof(struct got_entry *) * size);
      if (local_got_ents == NULL)
	return FALSE;

      memset (local_got_ents, 0, sizeof(struct got_entry *) * size);
      elf_local_got_ents (abfd) = local_got_ents;
    }

  return local_got_ents;
}

/* Relocate an arc ELF section.
   Function : elf_arc_relocate_section
   Brief    : Relocate an arc section, by handling all the relocations
	     appearing in that section.
   Args     : output_bfd    : The bfd being written to.
	      info	    : Link information.
	      input_bfd     : The input bfd.
	      input_section : The section being relocated.
	      contents	    : contents of the section being relocated.
	      relocs	    : List of relocations in the section.
	      local_syms    : is a pointer to the swapped in local symbols.
	      local_section : is an array giving the section in the input file
			      corresponding to the st_shndx field of each
			      local symbol.  */
static bfd_boolean
elf_arc_relocate_section (bfd *		   output_bfd,
			  struct bfd_link_info *  info,
			  bfd *		   input_bfd,
			  asection *	      input_section,
			  bfd_byte *	      contents,
			  Elf_Internal_Rela *     relocs,
			  Elf_Internal_Sym *      local_syms,
			  asection **	     local_sections)
{
  Elf_Internal_Shdr *	   symtab_hdr;
  struct elf_link_hash_entry ** sym_hashes;
  struct got_entry **	   local_got_ents;
  Elf_Internal_Rela *	   rel;
  Elf_Internal_Rela *	   relend;
  struct elf_link_hash_table *htab = elf_hash_table (info);

  symtab_hdr = &((elf_tdata (input_bfd))->symtab_hdr);
  sym_hashes = elf_sym_hashes (input_bfd);

  rel = relocs;
  relend = relocs + input_section->reloc_count;
  for (; rel < relend; rel++)
    {
      enum elf_arc_reloc_type       r_type;
      reloc_howto_type *	    howto;
      unsigned long		 r_symndx;
      struct elf_link_hash_entry *  h;
      Elf_Internal_Sym *	    sym;
      asection *		    sec;
      struct elf_link_hash_entry *h2;

      struct arc_relocation_data reloc_data =
      {
	.reloc_offset = 0,
	.reloc_addend = 0,
	.got_offset_value = 0,
	.sym_value = 0,	
	.sym_section = NULL,
	.howto = NULL,
	.input_section = NULL,
	.sdata_begin_symbol_vma = 0,
	.sdata_begin_symbol_vma_set = FALSE,
	.got_symbol_vma = 0,
	.should_relocate = FALSE
      };

      r_type = ELF32_R_TYPE (rel->r_info);

      if (r_type >= (int) R_ARC_max)
	{
	  bfd_set_error (bfd_error_bad_value);
	  return FALSE;
	}
      howto = &elf_arc_howto_table[r_type];

      r_symndx = ELF32_R_SYM (rel->r_info);

      /* If we are generating another .o file and the symbol in not
	 local, skip this relocation.  */
      if (bfd_link_relocatable (info))
	{
	  /* This is a relocateable link.  We don't have to change
	     anything, unless the reloc is against a section symbol,
	     in which case we have to adjust according to where the
	     section symbol winds up in the output section.  */

	  /* Checks if this is a local symbol and thus the reloc
	     might (will??) be against a section symbol.  */
	  if (r_symndx < symtab_hdr->sh_info)
	    {
	      sym = local_syms + r_symndx;
	      if (ELF_ST_TYPE (sym->st_info) == STT_SECTION)
		{
		  sec = local_sections[r_symndx];

		  /* for RELA relocs.Just adjust the addend
		     value in the relocation entry.  */
		  rel->r_addend += sec->output_offset + sym->st_value;

		  BFD_DEBUG_PIC (
		    PR_DEBUG ("local symbols reloc "
			      "(section=%d %s) seen in %s\n",
			      r_symndx,
			      local_sections[r_symndx]->name,
			      __PRETTY_FUNCTION__)
		  );
		}
	    }

	  continue;
	}

      h2 = elf_link_hash_lookup (elf_hash_table (info), "__SDATA_BEGIN__",
				 FALSE, FALSE, TRUE);

      if (reloc_data.sdata_begin_symbol_vma_set == FALSE
	    && h2 != NULL && h2->root.type != bfd_link_hash_undefined
	    && h2->root.u.def.section->output_section != NULL)
	/* TODO: Verify this condition.  */
	{
	  reloc_data.sdata_begin_symbol_vma =
	    (h2->root.u.def.value +
	     h2->root.u.def.section->output_section->vma);
	  reloc_data.sdata_begin_symbol_vma_set = TRUE;
	}

      reloc_data.input_section = input_section;
      reloc_data.howto = howto;
      reloc_data.reloc_offset = rel->r_offset;
      reloc_data.reloc_addend = rel->r_addend;

      /* This is a final link.  */
      h = NULL;
      sym = NULL;
      sec = NULL;

      if (r_symndx < symtab_hdr->sh_info) /* A local symbol.  */
	{
	  local_got_ents = arc_get_local_got_ents (output_bfd);
	  struct got_entry *entry = local_got_ents[r_symndx];

	  sym = local_syms + r_symndx;
	  sec = local_sections[r_symndx];

	  reloc_data.sym_value = sym->st_value;
	  reloc_data.sym_section = sec;

	  /* Mergeable section handling.  */
	  if ((sec->flags & SEC_MERGE)
	      && ELF_ST_TYPE (sym->st_info) == STT_SECTION)
	    {
	      asection *msec;
	      msec = sec;
	      rel->r_addend = _bfd_elf_rel_local_sym (output_bfd, sym,
						      &msec, rel->r_addend);
	      rel->r_addend -= (sec->output_section->vma
				+ sec->output_offset
				+ sym->st_value);
	      rel->r_addend += msec->output_section->vma + msec->output_offset;

	      reloc_data.reloc_addend = rel->r_addend;
	    }

	  if ((is_reloc_for_GOT (howto)
	       || is_reloc_for_TLS (howto)) && entry != NULL)
	    {
	      if (is_reloc_for_TLS (howto))
		while (entry->type == GOT_NORMAL && entry->next != NULL)
		  entry = entry->next;

	      if (is_reloc_for_GOT (howto))
		while (entry->type != GOT_NORMAL && entry->next != NULL)
		  entry = entry->next;

	      if (entry->type == GOT_TLS_GD && entry->processed == FALSE)
		{
		  bfd_vma sym_vma = sym->st_value
				    + sec->output_section->vma
				    + sec->output_offset;

		  /* Create dynamic relocation for local sym.  */
		  ADD_RELA (output_bfd, got, entry->offset, 0,
			    R_ARC_TLS_DTPMOD, 0);
		  ADD_RELA (output_bfd, got, entry->offset+4, 0,
			    R_ARC_TLS_DTPOFF, 0);

		  bfd_vma sec_vma = sec->output_section->vma
				    + sec->output_offset;
		  bfd_put_32 (output_bfd, sym_vma - sec_vma,
			      htab->sgot->contents + entry->offset + 4);

		  ARC_DEBUG ("arc_info: FIXED -> GOT_TLS_GD value "
			 "= 0x%x @ 0x%x, for symbol %s\n",
			 sym_vma - sec_vma,
			 htab->sgot->contents + entry->offset + 4,
			 "(local)");

		  entry->processed = TRUE;
		}
	      if (entry->type == GOT_TLS_IE && entry->processed == FALSE)
		{
		  bfd_vma sym_vma = sym->st_value
				    + sec->output_section->vma
				    + sec->output_offset;
		  bfd_vma sec_vma = htab->tls_sec->output_section->vma;
		  bfd_put_32 (output_bfd, sym_vma - sec_vma,
			      htab->sgot->contents + entry->offset);
		  /* TODO: Check if this type of relocs is the cause
		     for all the ARC_NONE dynamic relocs.  */

		  ARC_DEBUG ("arc_info: FIXED -> GOT_TLS_IE value = "
			 "0x%x @ 0x%x, for symbol %s\n",
			 sym_vma - sec_vma,
			 htab->sgot->contents + entry->offset,
			 "(local)");

		  entry->processed = TRUE;
		}
	      if (entry->type == GOT_NORMAL && entry->processed == FALSE)
		{
		  bfd_vma sec_vma = reloc_data.sym_section->output_section->vma
				    + reloc_data.sym_section->output_offset;

		  bfd_put_32 (output_bfd, reloc_data.sym_value + sec_vma,
			      htab->sgot->contents + entry->offset);

		  ARC_DEBUG ("arc_info: PATCHED: 0x%08x @ 0x%08x for "
			 "sym %s in got offset 0x%x\n",
			 reloc_data.sym_value + sec_vma,
			 htab->sgot->output_section->vma
			 + htab->sgot->output_offset + entry->offset,
			 "(local)",
			 entry->offset);
		  entry->processed = TRUE;
		}

	      reloc_data.got_offset_value = entry->offset;
	      ARC_DEBUG ("arc_info: GOT_ENTRY = %d, offset = 0x%x, "
		     "vma = 0x%x for symbol %s\n",
		     entry->type, entry->offset,
		     htab->sgot->output_section->vma
		     + htab->sgot->output_offset + entry->offset,
		     "(local)");
	    }

	  BFD_ASSERT (htab->sgot != NULL || !is_reloc_for_GOT (howto));
	  if (htab->sgot != NULL)
	    reloc_data.got_symbol_vma = htab->sgot->output_section->vma
					+ htab->sgot->output_offset;

	  reloc_data.should_relocate = TRUE;
	}
      else /* Global symbol.  */
	{
	  /* Get the symbol's entry in the symtab.  */
	  h = sym_hashes[r_symndx - symtab_hdr->sh_info];

	  while (h->root.type == bfd_link_hash_indirect
		 || h->root.type == bfd_link_hash_warning)
	    h = (struct elf_link_hash_entry *) h->root.u.i.link;

	  BFD_ASSERT ((h->dynindx == -1) >= (h->forced_local != 0));
	  /* If we have encountered a definition for this symbol.  */
	  if (h->root.type == bfd_link_hash_defined
	      || h->root.type == bfd_link_hash_defweak)
	    {
	      reloc_data.sym_value = h->root.u.def.value;
	      reloc_data.sym_section = h->root.u.def.section;

	      reloc_data.should_relocate = TRUE;

	      if (is_reloc_for_GOT (howto) && !bfd_link_pic (info))
		{
		  /* TODO: Change it to use arc_do_relocation with
		    ARC_32 reloc.  Try to use ADD_RELA macro.  */
		  bfd_vma relocation =
		    reloc_data.sym_value + reloc_data.reloc_addend
		    + (reloc_data.sym_section->output_section != NULL ?
			(reloc_data.sym_section->output_offset
			 + reloc_data.sym_section->output_section->vma)
		      : 0);

		  BFD_ASSERT (h->got.glist);
		  bfd_vma got_offset = h->got.glist->offset;
		  bfd_put_32 (output_bfd, relocation,
			      htab->sgot->contents + got_offset);
		}
	      if (is_reloc_for_PLT (howto) && h->plt.offset != (bfd_vma) -1)
		{
		  /* TODO: This is repeated up here.  */
		  reloc_data.sym_value = h->plt.offset;
		  reloc_data.sym_section = htab->splt;
		}
	    }
	  else if (h->root.type == bfd_link_hash_undefweak)
	    {
	      /* Is weak symbol and has no definition.  */
	      if (is_reloc_for_GOT (howto))
		{
		  reloc_data.sym_value = h->root.u.def.value;
		  reloc_data.sym_section = htab->sgot;
		  reloc_data.should_relocate = TRUE;
		}
	      else if (is_reloc_for_PLT (howto)
		       && h->plt.offset != (bfd_vma) -1)
		{
		  /* TODO: This is repeated up here.  */
		  reloc_data.sym_value = h->plt.offset;
		  reloc_data.sym_section = htab->splt;
		  reloc_data.should_relocate = TRUE;
		}
	      else
		continue;
	    }
	  else
	    {
	      if (is_reloc_for_GOT (howto))
		{
		  reloc_data.sym_value = h->root.u.def.value;
		  reloc_data.sym_section = htab->sgot;

		  reloc_data.should_relocate = TRUE;
		}
	      else if (is_reloc_for_PLT (howto))
		{
		  /* Fail if it is linking for PIE and the symbol is
		     undefined.  */
		  if (bfd_link_executable (info)
		      && !(*info->callbacks->undefined_symbol)
		        (info, h->root.root.string, input_bfd, input_section,
		         rel->r_offset, TRUE))
		    {
		      return FALSE;
		    }
		  reloc_data.sym_value = h->plt.offset;
		  reloc_data.sym_section = htab->splt;

		  reloc_data.should_relocate = TRUE;
		}
	      else if (!bfd_link_pic (info)
		       && !(*info->callbacks->undefined_symbol)
		       (info, h->root.root.string, input_bfd, input_section,
			rel->r_offset, TRUE))
		{
		  return FALSE;
		}
	    }

	  if (h->got.glist != NULL)
	    {
	      struct got_entry *entry = h->got.glist;

	      if (is_reloc_for_GOT (howto) || is_reloc_for_TLS (howto))
		{
		  if (! elf_hash_table (info)->dynamic_sections_created
		      || (bfd_link_pic (info)
			  && SYMBOL_REFERENCES_LOCAL (info, h)))
		    {
		      reloc_data.sym_value = h->root.u.def.value;
		      reloc_data.sym_section = h->root.u.def.section;

		      if (is_reloc_for_TLS (howto))
			while (entry->type == GOT_NORMAL && entry->next != NULL)
			  entry = entry->next;

		      if (entry->processed == FALSE
			  && (entry->type == GOT_TLS_GD
			      || entry->type == GOT_TLS_IE))
			{
			  bfd_vma sym_value = h->root.u.def.value
			    + h->root.u.def.section->output_section->vma
			    + h->root.u.def.section->output_offset;

			  bfd_vma sec_vma =
			    elf_hash_table (info)->tls_sec->output_section->vma;

			  bfd_put_32 (output_bfd,
				      sym_value - sec_vma,
				      htab->sgot->contents + entry->offset
				      + (entry->existing_entries == TLS_GOT_MOD_AND_OFF ? 4 : 0));

			  ARC_DEBUG ("arc_info: FIXED -> %s value = 0x%x "
				     "@ 0x%x, for symbol %s\n",
				     (entry->type == GOT_TLS_GD ? "GOT_TLS_GD" :
				      "GOT_TLS_IE"),
				     sym_value - sec_vma,
				     htab->sgot->contents + entry->offset
				     + (entry->existing_entries == TLS_GOT_MOD_AND_OFF ? 4 : 0),
				     h->root.root.string);

			  entry->processed = TRUE;
			}

		      if (entry->type == GOT_TLS_IE && entry->processed == FALSE)
			{
			  bfd_vma sec_vma = htab->tls_sec->output_section->vma;
			  bfd_put_32 (output_bfd,
				      reloc_data.sym_value - sec_vma,
				      htab->sgot->contents + entry->offset);
			}

		      if (entry->type == GOT_NORMAL && entry->processed == FALSE)
			{
			  bfd_vma sec_vma =
			    reloc_data.sym_section->output_section->vma
			    + reloc_data.sym_section->output_offset;

			  if (h->root.type != bfd_link_hash_undefweak)
			    {
			      bfd_put_32 (output_bfd,
					  reloc_data.sym_value + sec_vma,
					  htab->sgot->contents + entry->offset);

			      ARC_DEBUG ("arc_info: PATCHED: 0x%08x "
					 "@ 0x%08x for sym %s in got offset 0x%x\n",
					 reloc_data.sym_value + sec_vma,
					 htab->sgot->output_section->vma
					 + htab->sgot->output_offset + entry->offset,
					 h->root.root.string,
					 entry->offset);
			    }
			  else
			    {
			      ARC_DEBUG ("arc_info: PATCHED: NOT_PATCHED "
					 "@ 0x%08x for sym %s in got offset 0x%x "
					 "(is undefweak)\n",
					 htab->sgot->output_section->vma
					 + htab->sgot->output_offset + entry->offset,
					 h->root.root.string,
					 entry->offset);
			    }

			  entry->processed = TRUE;
			}
		    }
		}

	      reloc_data.got_offset_value = entry->offset;

	      ARC_DEBUG ("arc_info: GOT_ENTRY = %d, offset = 0x%x, "
			 "vma = 0x%x for symbol %s\n",
			 entry->type, entry->offset,
			 htab->sgot->output_section->vma
			 + htab->sgot->output_offset + entry->offset,
			 h->root.root.string);
	    }

	  BFD_ASSERT (htab->sgot != NULL || !is_reloc_for_GOT (howto));
	  if (htab->sgot != NULL)
	    reloc_data.got_symbol_vma = htab->sgot->output_section->vma
					+ htab->sgot->output_offset;
	}

      switch (r_type)
	{
	  case R_ARC_32:
	  case R_ARC_32_ME:
	  case R_ARC_PC32:
	  case R_ARC_32_PCREL:
	    if (bfd_link_pic (info) && !bfd_link_pie (info)
		&& ((r_type != R_ARC_PC32 && r_type != R_ARC_32_PCREL)
		    || (h != NULL
			&& h->dynindx != -1
			&& (!info->symbolic || !h->def_regular))))
	      {
		Elf_Internal_Rela outrel;
		bfd_byte *loc;
		bfd_boolean skip = FALSE;
		bfd_boolean relocate = FALSE;
		asection *sreloc = _bfd_elf_get_dynamic_reloc_section
				 (input_bfd, input_section,
				  /*RELA*/ TRUE);

		BFD_ASSERT (sreloc != NULL);

		outrel.r_offset = _bfd_elf_section_offset (output_bfd,
							   info,
							   input_section,
							   rel->r_offset);
		if (outrel.r_offset == (bfd_vma) -1)
		  skip = TRUE;

		outrel.r_addend = rel->r_addend;
		outrel.r_offset += (input_section->output_section->vma
				    + input_section->output_offset);

		if (skip)
		  {
		    memset (&outrel, 0, sizeof outrel);
		    relocate = FALSE;
		  }
		else if (r_type == R_ARC_PC32
			 || r_type == R_ARC_32_PCREL)
		  {
		    BFD_ASSERT (h != NULL);
		    if ((input_section->flags & SEC_ALLOC) != 0)
		      relocate = FALSE;
		    else
		      relocate = TRUE;

		    BFD_ASSERT (h->dynindx != -1);
		    outrel.r_info = ELF32_R_INFO (h->dynindx, r_type);
		  }
		else
		  {
		    /* Handle local symbols, they either do not have a
		       global hash table entry (h == NULL), or are
		       forced local due to a version script
		       (h->forced_local), or the third condition is
		       legacy, it appears to say something like, for
		       links where we are pre-binding the symbols, or
		       there's not an entry for this symbol in the
		       dynamic symbol table, and it's a regular symbol
		       not defined in a shared object, then treat the
		       symbol as local, resolve it now.  */
		    if (h == NULL
			|| ((info->symbolic || h->dynindx == -1)
			    && h->def_regular)
			|| h->forced_local)
		      {
			relocate = TRUE;
			/* outrel.r_addend = 0; */
			outrel.r_info = ELF32_R_INFO (0, R_ARC_RELATIVE);
		      }
		    else
		      {
			BFD_ASSERT (h->dynindx != -1);

			/* This type of dynamic relocation cannot be created
			   for code sections.  */
			BFD_ASSERT ((input_section->flags & SEC_CODE) == 0);

			if ((input_section->flags & SEC_ALLOC) != 0)
			  relocate = FALSE;
			else
			  relocate = TRUE;

			BFD_ASSERT (h->dynindx != -1);
			outrel.r_info = ELF32_R_INFO (h->dynindx, R_ARC_32);
		      }
		  }

		BFD_ASSERT (sreloc->contents != 0);

		loc = sreloc->contents;
		loc += sreloc->reloc_count * sizeof (Elf32_External_Rela);
		sreloc->reloc_count += 1;

		bfd_elf32_swap_reloca_out (output_bfd, &outrel, loc);

		if (relocate == FALSE)
		  continue;
	      }
	    break;
	  default:
	    break;
	}

      if (is_reloc_SDA_relative (howto)
	  && (reloc_data.sdata_begin_symbol_vma_set == FALSE))
	{
	  (*_bfd_error_handler)
	      ("Error: Linker symbol __SDATA_BEGIN__ not found");
	  bfd_set_error (bfd_error_bad_value);
	  return FALSE;
	}

      DEBUG_ARC_RELOC (reloc_data);

      if (arc_do_relocation (contents, reloc_data, info) != bfd_reloc_ok)
	return FALSE;
    }

  return TRUE;
}

static struct dynamic_sections
arc_create_dynamic_sections (bfd * abfd, struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab;
  bfd	 *dynobj;
  struct dynamic_sections ds =
    {
      .initialized = FALSE,
      .sgot = NULL,
      .srelgot = NULL,
      .sgotplt = NULL,
      .srelgotplt = NULL,
      .sdyn = NULL,
      .splt = NULL,
      .srelplt = NULL
    };

  htab = elf_hash_table (info);
  BFD_ASSERT (htab);

  /* Create dynamic sections for relocatable executables so that we
     can copy relocations.  */
  if (! htab->dynamic_sections_created && bfd_link_pic (info))
    {
      if (! _bfd_elf_link_create_dynamic_sections (abfd, info))
	BFD_ASSERT (0);
    }

  dynobj = (elf_hash_table (info))->dynobj;

  if (dynobj)
    {
      ds.sgot = htab->sgot;
      ds.srelgot = htab->srelgot;

      ds.sgotplt = bfd_get_section_by_name (dynobj, ".got.plt");
      ds.srelgotplt = ds.srelplt;

      ds.splt = bfd_get_section_by_name (dynobj, ".plt");
      ds.srelplt = bfd_get_section_by_name (dynobj, ".rela.plt");
    }

  if (htab->dynamic_sections_created)
    {
      ds.sdyn = bfd_get_section_by_name (dynobj, ".dynamic");
    }

  ds.initialized = TRUE;

  return ds;
}

#define ADD_SYMBOL_REF_SEC_AND_RELOC(SECNAME, COND_FOR_RELOC, H)	\
  htab->s##SECNAME->size;						\
  {									\
    if (COND_FOR_RELOC)							\
      {									\
	htab->srel##SECNAME->size += sizeof (Elf32_External_Rela);	\
	  ARC_DEBUG ("arc_info: Added reloc space in "			\
		     #SECNAME " section at " __FILE__			\
		     ":%d for symbol\n",				\
		     __LINE__, name_for_global_symbol (H));		\
      }									\
    if (H)								\
      if (h->dynindx == -1 && !h->forced_local)				\
	if (! bfd_elf_link_record_dynamic_symbol (info, H))		\
	  return FALSE;							\
     htab->s##SECNAME->size += 4;			\
   }

static bfd_boolean
elf_arc_check_relocs (bfd *		         abfd,
		      struct bfd_link_info *     info,
		      asection *		 sec,
		      const Elf_Internal_Rela *  relocs)
{
  Elf_Internal_Shdr *		symtab_hdr;
  struct elf_link_hash_entry **	sym_hashes;
  struct got_entry **		local_got_ents;
  const Elf_Internal_Rela *	rel;
  const Elf_Internal_Rela *	rel_end;
  bfd *				dynobj;
  asection *			sreloc = NULL;
  struct elf_link_hash_table *  htab = elf_hash_table (info);

  if (bfd_link_relocatable (info))
    return TRUE;

  dynobj = (elf_hash_table (info))->dynobj;
  symtab_hdr = &((elf_tdata (abfd))->symtab_hdr);
  sym_hashes = elf_sym_hashes (abfd);
  local_got_ents = arc_get_local_got_ents (abfd);

  rel_end = relocs + sec->reloc_count;
  for (rel = relocs; rel < rel_end; rel++)
    {
      enum elf_arc_reloc_type r_type;
      reloc_howto_type *howto;
      unsigned long   r_symndx;
      struct elf_link_hash_entry *h;

      r_type = ELF32_R_TYPE (rel->r_info);

      if (r_type >= (int) R_ARC_max)
	{
	  bfd_set_error (bfd_error_bad_value);
	  return FALSE;
	}
      howto = &elf_arc_howto_table[r_type];

      if (dynobj == NULL
	  && (is_reloc_for_GOT (howto) == TRUE
	      || is_reloc_for_TLS (howto) == TRUE))
	{
	  dynobj = elf_hash_table (info)->dynobj = abfd;
	  if (! _bfd_elf_create_got_section (abfd, info))
	    return FALSE;
	}

      /* Load symbol information.  */
      r_symndx = ELF32_R_SYM (rel->r_info);
      if (r_symndx < symtab_hdr->sh_info) /* Is a local symbol.  */
	h = NULL;
      else /* Global one.  */
	h = sym_hashes[r_symndx - symtab_hdr->sh_info];

      switch (r_type)
	{
	  case R_ARC_32:
	  case R_ARC_32_ME:
	    /* During shared library creation, these relocs should not
	       appear in a shared library (as memory will be read only
	       and the dynamic linker can not resolve these.  However
	       the error should not occur for e.g. debugging or
	       non-readonly sections.  */
	    if (bfd_link_dll (info) && !bfd_link_pie (info)
		&& (sec->flags & SEC_ALLOC) != 0
		&& (sec->flags & SEC_READONLY) == 0
		&& (sec->flags & SEC_CODE) != 0)
	      {
		const char *name;
		if (h)
		  name = h->root.root.string;
		else
		  /* bfd_elf_sym_name (abfd, symtab_hdr, isym, NULL);  */
		  name = "UNKNOWN";
		(*_bfd_error_handler)
		  (_("\
%B: relocation %s against `%s' can not be used when making a shared object; recompile with -fPIC"),
		    abfd,
		    arc_elf_howto (r_type)->name,
		    name);
		bfd_set_error (bfd_error_bad_value);
		return FALSE;
	      }

	    /* In some cases we are not setting the 'non_got_ref'
	       flag, even though the relocations don't require a GOT
	       access.  We should extend the testing in this area to
	       ensure that no significant cases are being missed.  */
	    if (h)
	      h->non_got_ref = 1;
	    /* FALLTHROUGH */
	  case R_ARC_PC32:
	  case R_ARC_32_PCREL:
	    if (bfd_link_pic (info) && !bfd_link_pie (info)
		&& ((r_type != R_ARC_PC32 && r_type != R_ARC_32_PCREL)
		    || (h != NULL
			&& h->dynindx != -1
			&& (!info->symbolic || !h->def_regular))))
	      {
		if (sreloc == NULL)
		  {
		    sreloc = _bfd_elf_make_dynamic_reloc_section (sec, dynobj,
								  2, abfd,
								  /*rela*/
								  TRUE);

		    if (sreloc == NULL)
		      return FALSE;
		  }
		sreloc->size += sizeof (Elf32_External_Rela);

	      }
	  default:
	    break;
	}

      if (is_reloc_for_PLT (howto) == TRUE)
	{
	  if (h == NULL)
	    continue;
	  else
	    h->needs_plt = 1;
	}

      if (is_reloc_for_GOT (howto) == TRUE)
	{
	  if (h == NULL)
	    {
	      /* Local symbol.  */
	      if (local_got_ents[r_symndx] == NULL)
		{
		  bfd_vma offset =
		    ADD_SYMBOL_REF_SEC_AND_RELOC (got,
						  bfd_link_pic (info),
						  NULL);
		  new_got_entry_to_list (&(local_got_ents[r_symndx]),
					 GOT_NORMAL, offset, TLS_GOT_NONE);
		}
	    }
	  else
	    {
	      /* Global symbol.  */
	      h = sym_hashes[r_symndx - symtab_hdr->sh_info];
	      if (h->got.glist == NULL)
		{
		  bfd_vma offset =
		    ADD_SYMBOL_REF_SEC_AND_RELOC (got, TRUE, h);
		  new_got_entry_to_list (&h->got.glist,
					 GOT_NORMAL, offset, TLS_GOT_NONE);
		}
	    }
	}

      if (is_reloc_for_TLS (howto) == TRUE)
	{
	  enum tls_type_e type = GOT_UNKNOWN;

	  switch (r_type)
	    {
	      case R_ARC_TLS_GD_GOT:
		type = GOT_TLS_GD;
		break;
	      case R_ARC_TLS_IE_GOT:
		type = GOT_TLS_IE;
		break;
	      default:
		break;
	    }

	  struct got_entry **list = NULL;
	  if (h != NULL)
	    list = &(h->got.glist);
	  else
	    list = &(local_got_ents[r_symndx]);

	  if (type != GOT_UNKNOWN && !symbol_has_entry_of_type (*list, type))
	    {
	      enum tls_got_entries entries = TLS_GOT_NONE;
	      bfd_vma offset =
		ADD_SYMBOL_REF_SEC_AND_RELOC (got, TRUE, h);

	      if (type == GOT_TLS_GD)
		{
		  bfd_vma ATTRIBUTE_UNUSED notneeded =
		    ADD_SYMBOL_REF_SEC_AND_RELOC (got, TRUE, h);
		  entries = TLS_GOT_MOD_AND_OFF;
		}

	      if (entries == TLS_GOT_NONE)
		entries = TLS_GOT_OFF;

	      new_got_entry_to_list (list, type, offset, entries);
	    }
	}
    }

  return TRUE;
}

#define ELF_DYNAMIC_INTERPRETER  "/sbin/ld-uClibc.so"

static struct plt_version_t *
arc_get_plt_version (struct bfd_link_info *info)
{
  int i;

  for (i = 0; i < 1; i++)
    {
      ARC_DEBUG ("%d: size1 = %d, size2 = %d\n", i,
		 plt_versions[i].entry_size,
		 plt_versions[i].elem_size);
    }

  if (bfd_get_mach (info->output_bfd) == bfd_mach_arc_arcv2)
    {
      if (bfd_link_pic (info))
	return &(plt_versions[ELF_ARCV2_PIC]);
      else
	return &(plt_versions[ELF_ARCV2_ABS]);
    }
  else
    {
      if (bfd_link_pic (info))
	return &(plt_versions[ELF_ARC_PIC]);
      else
	return &(plt_versions[ELF_ARC_ABS]);
    }
}

static bfd_vma
add_symbol_to_plt (struct bfd_link_info *info)
{
  struct elf_link_hash_table *htab = elf_hash_table (info);
  bfd_vma ret;

  struct plt_version_t *plt_data = arc_get_plt_version (info);

  /* If this is the first .plt entry, make room for the special first
     entry.  */
  if (htab->splt->size == 0)
    htab->splt->size += plt_data->entry_size;

  ret = htab->splt->size;

  htab->splt->size += plt_data->elem_size;
  ARC_DEBUG ("PLT_SIZE = %d\n", htab->splt->size);

  htab->sgotplt->size += 4;
  htab->srelplt->size += sizeof (Elf32_External_Rela);

  return ret;
}

#define PLT_DO_RELOCS_FOR_ENTRY(ABFD, DS, RELOCS)	\
  plt_do_relocs_for_symbol (ABFD, DS, RELOCS, 0, 0)

static void
plt_do_relocs_for_symbol (bfd *abfd,
			  struct elf_link_hash_table *htab,
			  const struct plt_reloc *reloc,
			  bfd_vma plt_offset,
			  bfd_vma symbol_got_offset)
{
  while (SYM_ONLY (reloc->symbol) != LAST_RELOC)
    {
      bfd_vma relocation = 0;

      switch (SYM_ONLY (reloc->symbol))
	{
	  case SGOT:
		relocation =
		    htab->sgotplt->output_section->vma +
		    htab->sgotplt->output_offset + symbol_got_offset;
		break;
	}
      relocation += reloc->addend;

      if (IS_RELATIVE (reloc->symbol))
	{
	  bfd_vma reloc_offset = reloc->offset;
	  reloc_offset -= (IS_INSN_32 (reloc->symbol)) ? 4 : 0;
	  reloc_offset -= (IS_INSN_24 (reloc->symbol)) ? 2 : 0;

	  relocation -= htab->splt->output_section->vma
			 + htab->splt->output_offset
			 + plt_offset + reloc_offset;
	}

      /* TODO: being ME is not a property of the relocation but of the
	 section of which is applying the relocation. */
      if (IS_MIDDLE_ENDIAN (reloc->symbol) && !bfd_big_endian (abfd))
	{
	  relocation =
	      ((relocation & 0xffff0000) >> 16) |
	      ((relocation & 0xffff) << 16);
	}

      switch (reloc->size)
	{
	  case 32:
	    bfd_put_32 (htab->splt->output_section->owner,
			relocation,
			htab->splt->contents + plt_offset + reloc->offset);
	    break;
	}

      reloc = &(reloc[1]); /* Jump to next relocation.  */
    }
}

static void
relocate_plt_for_symbol (bfd *output_bfd,
			 struct bfd_link_info *info,
			 struct elf_link_hash_entry *h)
{
  struct plt_version_t *plt_data = arc_get_plt_version (info);
  struct elf_link_hash_table *htab = elf_hash_table (info);

  bfd_vma plt_index = (h->plt.offset  - plt_data->entry_size)
		      / plt_data->elem_size;
  bfd_vma got_offset = (plt_index + 3) * 4;

  ARC_DEBUG ("arc_info: PLT_OFFSET = 0x%x, PLT_ENTRY_VMA = 0x%x, \
GOT_ENTRY_OFFSET = 0x%x, GOT_ENTRY_VMA = 0x%x, for symbol %s\n",
	     h->plt.offset,
	     htab->splt->output_section->vma
	     + htab->splt->output_offset
	     + h->plt.offset,
	     got_offset,
	     htab->sgotplt->output_section->vma
	     + htab->sgotplt->output_offset
	     + got_offset,
	     h->root.root.string);


  {
    bfd_vma i = 0;
    uint16_t *ptr = (uint16_t *) plt_data->elem;
    for (i = 0; i < plt_data->elem_size/2; i++)
      {
	uint16_t data = ptr[i];
	bfd_put_16 (output_bfd,
		    (bfd_vma) data,
		    htab->splt->contents + h->plt.offset + (i*2));
      }
  }

  plt_do_relocs_for_symbol (output_bfd, htab,
			    plt_data->elem_relocs,
			    h->plt.offset,
			    got_offset);

  /* Fill in the entry in the global offset table.  */
  bfd_put_32 (output_bfd,
	      (bfd_vma) (htab->splt->output_section->vma
			 + htab->splt->output_offset),
	      htab->sgotplt->contents + got_offset);

  /* TODO: Fill in the entry in the .rela.plt section.  */
  {
    Elf_Internal_Rela rel;
    bfd_byte *loc;

    rel.r_offset = (htab->sgotplt->output_section->vma
		    + htab->sgotplt->output_offset
		    + got_offset);
    rel.r_addend = 0;

    BFD_ASSERT (h->dynindx != -1);
    rel.r_info = ELF32_R_INFO (h->dynindx, R_ARC_JMP_SLOT);

    loc = htab->srelplt->contents;
    loc += plt_index * sizeof (Elf32_External_Rela); /* relA */
    bfd_elf32_swap_reloca_out (output_bfd, &rel, loc);
  }
}

static void
relocate_plt_for_entry (bfd *abfd,
			struct bfd_link_info *info)
{
  struct plt_version_t *plt_data = arc_get_plt_version (info);
  struct elf_link_hash_table *htab = elf_hash_table (info);

  {
    bfd_vma i = 0;
    uint16_t *ptr = (uint16_t *) plt_data->entry;
    for (i = 0; i < plt_data->entry_size/2; i++)
      {
	uint16_t data = ptr[i];
	bfd_put_16 (abfd,
		    (bfd_vma) data,
		    htab->splt->contents + (i*2));
      }
  }
  PLT_DO_RELOCS_FOR_ENTRY (abfd, htab, plt_data->entry_relocs);
}

/* Desc : Adjust a symbol defined by a dynamic object and referenced
   by a regular object.  The current definition is in some section of
   the dynamic object, but we're not including those sections.  We
   have to change the definition to something the rest of the link can
   understand.  */

static bfd_boolean
elf_arc_adjust_dynamic_symbol (struct bfd_link_info *info,
			      struct elf_link_hash_entry *h)
{
  asection *s;
  unsigned int power_of_two;
  bfd *dynobj = (elf_hash_table (info))->dynobj;
  struct elf_link_hash_table *htab = elf_hash_table (info);

  if (h->type == STT_FUNC
      || h->type == STT_GNU_IFUNC
      || h->needs_plt == 1)
    {
      if (!bfd_link_pic (info) && !h->def_dynamic && !h->ref_dynamic)
	{
	  /* This case can occur if we saw a PLT32 reloc in an input
	     file, but the symbol was never referred to by a dynamic
	     object.  In such a case, we don't actually need to build
	     a procedure linkage table, and we can just do a PC32
	     reloc instead.  */
	  BFD_ASSERT (h->needs_plt);
	  return TRUE;
	}

      /* Make sure this symbol is output as a dynamic symbol.  */
      if (h->dynindx == -1 && !h->forced_local
	  && !bfd_elf_link_record_dynamic_symbol (info, h))
	return FALSE;

      if (bfd_link_pic (info)
	  || WILL_CALL_FINISH_DYNAMIC_SYMBOL (1, 0, h))
	{
	  bfd_vma loc = add_symbol_to_plt (info);

	  if (!bfd_link_pic (info) && !h->def_regular)
	    {
	      h->root.u.def.section = htab->splt;
	      h->root.u.def.value = loc;
	    }
	  h->plt.offset = loc;
	}
      else
	{
	  h->plt.offset = (bfd_vma) -1;
	  h->needs_plt = 0;
	}
      return TRUE;
    }

  /* If this is a weak symbol, and there is a real definition, the
     processor independent code will have arranged for us to see the
     real definition first, and we can just use the same value.  */
  if (h->u.weakdef != NULL)
    {
      BFD_ASSERT (h->u.weakdef->root.type == bfd_link_hash_defined
		  || h->u.weakdef->root.type == bfd_link_hash_defweak);
      h->root.u.def.section = h->u.weakdef->root.u.def.section;
      h->root.u.def.value = h->u.weakdef->root.u.def.value;
      return TRUE;
    }

  /* If there are no non-GOT references, we do not need a copy
     relocation.  */
  if (!h->non_got_ref)
    return TRUE;

  /* This is a reference to a symbol defined by a dynamic object which
     is not a function.  */

  /* If we are creating a shared library, we must presume that the
     only references to the symbol are via the global offset table.
     For such cases we need not do anything here; the relocations will
     be handled correctly by relocate_section.  */
  if (bfd_link_pic (info))
    return TRUE;

  /* We must allocate the symbol in our .dynbss section, which will
     become part of the .bss section of the executable.  There will be
     an entry for this symbol in the .dynsym section.  The dynamic
     object will contain position independent code, so all references
     from the dynamic object to this symbol will go through the global
     offset table.  The dynamic linker will use the .dynsym entry to
     determine the address it must put in the global offset table, so
     both the dynamic object and the regular object will refer to the
     same memory location for the variable.  */

  s = bfd_get_section_by_name (dynobj, ".dynbss");
  BFD_ASSERT (s != NULL);

  /* We must generate a R_ARC_COPY reloc to tell the dynamic linker to
     copy the initial value out of the dynamic object and into the
     runtime process image.  We need to remember the offset into the
     .rela.bss section we are going to use.  */
  if ((h->root.u.def.section->flags & SEC_ALLOC) != 0)
    {
      asection *srel;

      srel = bfd_get_section_by_name (dynobj, ".rela.bss");
      BFD_ASSERT (srel != NULL);
      srel->size += sizeof (Elf32_External_Rela);
      h->needs_copy = 1;
    }

  /* We need to figure out the alignment required for this symbol.  I
     have no idea how ELF linkers handle this.  */
  power_of_two = bfd_log2 (h->size);
  if (power_of_two > 3)
    power_of_two = 3;

  /* Apply the required alignment.  */
  s->size = BFD_ALIGN (s->size, (bfd_size_type) (1 << power_of_two));
  if (power_of_two > bfd_get_section_alignment (dynobj, s))
    {
      if (! bfd_set_section_alignment (dynobj, s, power_of_two))
	return FALSE;
    }

  /* Define the symbol as being at this point in the section.  */
  h->root.u.def.section = s;
  h->root.u.def.value = s->size;

  /* Increment the section size to make room for the symbol.  */
  s->size += h->size;

  return TRUE;
}

/* Function :  elf_arc_finish_dynamic_symbol
   Brief    :  Finish up dynamic symbol handling.  We set the
	     contents of various dynamic sections here.
   Args     :  output_bfd :
	       info	  :
	       h	  :
	       sym	  :
   Returns  : True/False as the return status.  */

static bfd_boolean
elf_arc_finish_dynamic_symbol (bfd * output_bfd,
			       struct bfd_link_info *info,
			       struct elf_link_hash_entry *h,
			       Elf_Internal_Sym * sym)
{
  if (h->plt.offset != (bfd_vma) -1)
    {
      relocate_plt_for_symbol (output_bfd, info, h);

      if (!h->def_regular)
	{
	  /* Mark the symbol as undefined, rather than as defined in
	     the .plt section.  Leave the value alone.  */
	  sym->st_shndx = SHN_UNDEF;
	}
    }

  if (h->got.glist != NULL)
    {
      struct got_entry *list = h->got.glist;

      /* Traverse the list of got entries for this symbol.  */
      while (list)
	{
	  bfd_vma got_offset = h->got.glist->offset;

	  if (list->type == GOT_NORMAL
	      && list->created_dyn_relocation == FALSE)
	    {
	      if (bfd_link_pic (info)
		  && (info->symbolic || h->dynindx == -1)
		  && h->def_regular)
		{
		  ADD_RELA (output_bfd, got, got_offset, 0, R_ARC_RELATIVE, 0);
		}
	      /* Do not fully understand the side effects of this condition.
		 The relocation space might still being reserved.  Perhaps
		 I should clear its value.  */
	      else if (h->dynindx != -1)
		{
		  ADD_RELA (output_bfd, got, got_offset, h->dynindx,
			  R_ARC_GLOB_DAT, 0);
		}
	      list->created_dyn_relocation = TRUE;
	    }
	  else if (list->existing_entries != TLS_GOT_NONE)
	    {
	      struct elf_link_hash_table *htab = elf_hash_table (info);
	      enum tls_got_entries e = list->existing_entries;

	      BFD_ASSERT (list->type != GOT_TLS_GD
			  || list->existing_entries == TLS_GOT_MOD_AND_OFF);

	      bfd_vma dynindx = h->dynindx == -1 ? 0 : h->dynindx;
	      if (e == TLS_GOT_MOD_AND_OFF || e == TLS_GOT_MOD)
		{
		  ADD_RELA (output_bfd, got, got_offset, dynindx,
			    R_ARC_TLS_DTPMOD, 0);
		  ARC_DEBUG ("arc_info: TLS_DYNRELOC: type = %d, \
GOT_OFFSET = 0x%x, GOT_VMA = 0x%x, INDEX = %d, ADDEND = 0x%x\n",
			     list->type,
			     got_offset,
			     htab->sgot->output_section->vma
			     + htab->sgot->output_offset + got_offset,
			     dynindx, 0);
		}
	      if (e == TLS_GOT_MOD_AND_OFF || e == TLS_GOT_OFF)
		{
		  bfd_vma addend = 0;
		  if (list->type == GOT_TLS_IE)
		    addend = bfd_get_32 (output_bfd,
					 htab->sgot->contents + got_offset);

		  ADD_RELA (output_bfd, got,
			    got_offset + (e == TLS_GOT_MOD_AND_OFF ? 4 : 0),
			    dynindx,
			    (list->type == GOT_TLS_IE ?
			     R_ARC_TLS_TPOFF : R_ARC_TLS_DTPOFF),
			    addend);

		  ARC_DEBUG ("arc_info: TLS_DYNRELOC: type = %d, \
GOT_OFFSET = 0x%x, GOT_VMA = 0x%x, INDEX = %d, ADDEND = 0x%x\n",
			     list->type,
			     got_offset,
			     htab->sgot->output_section->vma
			     + htab->sgot->output_offset + got_offset,
			     dynindx, addend);
		}
	    }

	  list = list->next;
	}

      h->got.glist = NULL;
    }

  if (h->needs_copy)
    {
      bfd_vma rel_offset = (h->root.u.def.value
			    + h->root.u.def.section->output_section->vma
			    + h->root.u.def.section->output_offset);

      asection *srelbss =
	bfd_get_section_by_name (h->root.u.def.section->owner,
				 ".rela.bss");

      bfd_byte * loc = srelbss->contents
	+ (srelbss->reloc_count * sizeof (Elf32_External_Rela));
      srelbss->reloc_count++;

      Elf_Internal_Rela rel;
      rel.r_addend = 0;
      rel.r_offset = rel_offset;

      BFD_ASSERT (h->dynindx != -1);
      rel.r_info = ELF32_R_INFO (h->dynindx, R_ARC_COPY);

      bfd_elf32_swap_reloca_out (output_bfd, &rel, loc);
    }

  /* Mark _DYNAMIC and _GLOBAL_OFFSET_TABLE_ as absolute.  */
  if (strcmp (h->root.root.string, "_DYNAMIC") == 0
      || strcmp (h->root.root.string, "__DYNAMIC") == 0
      || strcmp (h->root.root.string, "_GLOBAL_OFFSET_TABLE_") == 0)
    sym->st_shndx = SHN_ABS;

  return TRUE;
}

#define GET_SYMBOL_OR_SECTION(TAG, SYMBOL, SECTION, ASSERT)	\
  case TAG:							\
  if (SYMBOL != NULL)						\
    {								\
      h = elf_link_hash_lookup (elf_hash_table (info),		\
				SYMBOL, FALSE, FALSE, TRUE);	\
    }								\
  else if (SECTION != NULL)					\
    {								\
      s = bfd_get_section_by_name (output_bfd, SECTION);	\
      BFD_ASSERT (s != NULL || !ASSERT);			\
      do_it = TRUE;						\
    }								\
  break;

/* Function :  elf_arc_finish_dynamic_sections
   Brief    :  Finish up the dynamic sections handling.
   Args     :  output_bfd :
	       info	  :
	       h	  :
	       sym	  :
   Returns  : True/False as the return status.  */

static bfd_boolean
elf_arc_finish_dynamic_sections (bfd * output_bfd,
				 struct bfd_link_info *info)
{
  struct dynamic_sections ds = arc_create_dynamic_sections (output_bfd, info);
  struct elf_link_hash_table *htab = elf_hash_table (info);
  bfd *dynobj = (elf_hash_table (info))->dynobj;

  if (ds.sdyn)
    {
      Elf32_External_Dyn *dyncon, *dynconend;

      dyncon = (Elf32_External_Dyn *) ds.sdyn->contents;
      dynconend =
	(Elf32_External_Dyn *) (ds.sdyn->contents + ds.sdyn->size);
      for (; dyncon < dynconend; dyncon++)
	{
	  Elf_Internal_Dyn internal_dyn;
	  bfd_boolean	  do_it = FALSE;

	  struct elf_link_hash_entry *h = NULL;
	  asection	 *s = NULL;

	  bfd_elf32_swap_dyn_in (dynobj, dyncon, &internal_dyn);

	  switch (internal_dyn.d_tag)
	    {
	      GET_SYMBOL_OR_SECTION (DT_INIT, "_init", NULL, TRUE)
	      GET_SYMBOL_OR_SECTION (DT_FINI, "_fini", NULL, TRUE)
	      GET_SYMBOL_OR_SECTION (DT_PLTGOT, NULL, ".plt", TRUE)
	      GET_SYMBOL_OR_SECTION (DT_JMPREL, NULL, ".rela.plt", TRUE)
	      GET_SYMBOL_OR_SECTION (DT_PLTRELSZ, NULL, ".rela.plt", TRUE)
	      GET_SYMBOL_OR_SECTION (DT_RELASZ, NULL, ".rela.plt", FALSE)
	      GET_SYMBOL_OR_SECTION (DT_VERSYM, NULL, ".gnu.version", TRUE)
	      GET_SYMBOL_OR_SECTION (DT_VERDEF, NULL, ".gnu.version_d", TRUE)
	      GET_SYMBOL_OR_SECTION (DT_VERNEED, NULL, ".gnu.version_r", TRUE)
	      default:
		break;
	    }

	  /* In case the dynamic symbols should be updated with a symbol.  */
	  if (h != NULL
	      && (h->root.type == bfd_link_hash_defined
		  || h->root.type == bfd_link_hash_defweak))
	    {
	      asection	     *asec_ptr;

	      internal_dyn.d_un.d_val = h->root.u.def.value;
	      asec_ptr = h->root.u.def.section;
	      if (asec_ptr->output_section != NULL)
		{
		  internal_dyn.d_un.d_val +=
		    (asec_ptr->output_section->vma +
		     asec_ptr->output_offset);
		}
	      else
		{
		  /* The symbol is imported from another shared
		     library and does not apply to this one.  */
		  internal_dyn.d_un.d_val = 0;
		}
	      do_it = TRUE;
	    }
	  else if (s != NULL) /* With a section information.  */
	    {
	      switch (internal_dyn.d_tag)
		{
		  case DT_PLTGOT:
		  case DT_JMPREL:
		  case DT_VERSYM:
		  case DT_VERDEF:
		  case DT_VERNEED:
		    internal_dyn.d_un.d_ptr = s->vma;
		    do_it = TRUE;
		    break;

		  case DT_PLTRELSZ:
		    internal_dyn.d_un.d_val = s->size;
		    do_it = TRUE;
		    break;

		  case DT_RELASZ:
		    if (s != NULL)
		      internal_dyn.d_un.d_val -= s->size;
		    do_it = TRUE;
		    break;

		  default:
		    break;
		}
	    }

	  if (do_it == TRUE)
	    bfd_elf32_swap_dyn_out (output_bfd, &internal_dyn, dyncon);
	}

      if (htab->splt->size > 0)
	{
	  relocate_plt_for_entry (output_bfd, info);
	}

      /* TODO: Validate this.  */
      elf_section_data (htab->srelplt->output_section)->this_hdr.sh_entsize
	= 0xc;
    }

  /* Fill in the first three entries in the global offset table.  */
  if (htab->sgot)
    {
      if (htab->sgot->size > 0 || htab->sgotplt->size > 0)
	{
	  if (ds.sdyn == NULL)
	    bfd_put_32 (output_bfd, (bfd_vma) 0,
			htab->sgotplt->contents);
	  else
	    bfd_put_32 (output_bfd,
			ds.sdyn->output_section->vma + ds.sdyn->output_offset,
			htab->sgotplt->contents);
	  bfd_put_32 (output_bfd, (bfd_vma) 0, htab->sgotplt->contents + 4);
	  bfd_put_32 (output_bfd, (bfd_vma) 0, htab->sgotplt->contents + 8);
	}
    }

  return TRUE;
}

#define ADD_DYNAMIC_SYMBOL(NAME, TAG)					\
  h =  elf_link_hash_lookup (elf_hash_table (info),			\
			     NAME, FALSE, FALSE, FALSE);		\
  if ((h != NULL && (h->ref_regular || h->def_regular)))		\
    if (! _bfd_elf_add_dynamic_entry (info, TAG, 0))			\
      return FALSE;

/* Set the sizes of the dynamic sections.  */
static bfd_boolean
elf_arc_size_dynamic_sections (bfd * output_bfd,
			       struct bfd_link_info *info)
{
  bfd *	   dynobj;
  asection *      s;
  bfd_boolean	  relocs_exist = FALSE;
  bfd_boolean	  reltext_exist = FALSE;
  struct dynamic_sections ds = arc_create_dynamic_sections (output_bfd, info);
  struct elf_link_hash_table *htab = elf_hash_table (info);

  dynobj = (elf_hash_table (info))->dynobj;
  BFD_ASSERT (dynobj != NULL);

  if ((elf_hash_table (info))->dynamic_sections_created)
    {
      struct elf_link_hash_entry *h;

      /* Set the contents of the .interp section to the
	 interpreter.  */
      if (!bfd_link_pic (info))
	{
	  s = bfd_get_section_by_name (dynobj, ".interp");
	  BFD_ASSERT (s != NULL);
	  s->size = sizeof (ELF_DYNAMIC_INTERPRETER);
	  s->contents = (unsigned char *) ELF_DYNAMIC_INTERPRETER;
	}

      /* Add some entries to the .dynamic section.  We fill in some of
	 the values later, in elf_bfd_final_link, but we must add the
	 entries now so that we know the final size of the .dynamic
	 section.  Checking if the .init section is present.  We also
	 create DT_INIT and DT_FINI entries if the init_str has been
	 changed by the user.  */
      ADD_DYNAMIC_SYMBOL ("init", DT_INIT);
      ADD_DYNAMIC_SYMBOL ("fini", DT_FINI);
    }
  else
    {
      /* We may have created entries in the .rela.got section.
	 However, if we are not creating the dynamic sections, we will
	 not actually use these entries.  Reset the size of .rela.got,
	 which will cause it to get stripped from the output file
	 below.  */
      if (htab->srelgot != NULL)
	htab->srelgot->size = 0;
    }

  if (htab->splt != NULL && htab->splt->size == 0)
    htab->splt->flags |= SEC_EXCLUDE;
  for (s = dynobj->sections; s != NULL; s = s->next)
    {
      if ((s->flags & SEC_LINKER_CREATED) == 0)
	continue;

      if (strncmp (s->name, ".rela", 5) == 0)
	{
	  if (s->size == 0)
	    {
	      s->flags |= SEC_EXCLUDE;
	    }
	  else
	    {
	      if (strcmp (s->name, ".rela.plt") != 0)
		{
		  const char *outname =
		    bfd_get_section_name (output_bfd,
					  htab->srelplt->output_section);

		  asection *target = bfd_get_section_by_name (output_bfd,
							      outname + 4);

		  relocs_exist = TRUE;
		  if (target != NULL && target->size != 0
		      && (target->flags & SEC_READONLY) != 0
		      && (target->flags & SEC_ALLOC) != 0)
		    reltext_exist = TRUE;
		}
	    }

	  /* We use the reloc_count field as a counter if we need to
	     copy relocs into the output file.  */
	  s->reloc_count = 0;
	}

      if (strcmp (s->name, ".dynamic") == 0)
	continue;

      if (s->size != 0)
	s->contents = (bfd_byte *) bfd_zalloc (dynobj, s->size);

      if (s->contents == NULL && s->size != 0)
	return FALSE;
    }

  if (ds.sdyn)
    {
      /* TODO: Check if this is needed.  */
      if (!bfd_link_pic (info))
	if (!_bfd_elf_add_dynamic_entry (info, DT_DEBUG, 0))
		return FALSE;

      if (htab->splt && (htab->splt->flags & SEC_EXCLUDE) == 0)
	if (!_bfd_elf_add_dynamic_entry (info, DT_PLTGOT, 0)
	    || !_bfd_elf_add_dynamic_entry (info, DT_PLTRELSZ, 0)
	    || !_bfd_elf_add_dynamic_entry (info, DT_PLTREL, DT_RELA)
	    || !_bfd_elf_add_dynamic_entry (info, DT_JMPREL, 0)
	   )
	  return FALSE;

      if (relocs_exist == TRUE)
	if (!_bfd_elf_add_dynamic_entry (info, DT_RELA, 0)
	    || !_bfd_elf_add_dynamic_entry (info, DT_RELASZ, 0)
	    || !_bfd_elf_add_dynamic_entry (info, DT_RELENT,
					    sizeof (Elf32_External_Rela))
	   )
	  return FALSE;

      if (reltext_exist == TRUE)
	if (!_bfd_elf_add_dynamic_entry (info, DT_TEXTREL, 0))
	  return FALSE;
    }

  return TRUE;
}

const struct elf_size_info arc_elf32_size_info =
{
  sizeof (Elf32_External_Ehdr),
  sizeof (Elf32_External_Phdr),
  sizeof (Elf32_External_Shdr),
  sizeof (Elf32_External_Rel),
  sizeof (Elf32_External_Rela),
  sizeof (Elf32_External_Sym),
  sizeof (Elf32_External_Dyn),
  sizeof (Elf_External_Note),
  4,
  1,
  32, 2,
  ELFCLASS32, EV_CURRENT,
  bfd_elf32_write_out_phdrs,
  bfd_elf32_write_shdrs_and_ehdr,
  bfd_elf32_checksum_contents,
  bfd_elf32_write_relocs,
  bfd_elf32_swap_symbol_in,
  bfd_elf32_swap_symbol_out,
  bfd_elf32_slurp_reloc_table,
  bfd_elf32_slurp_symbol_table,
  bfd_elf32_swap_dyn_in,
  bfd_elf32_swap_dyn_out,
  bfd_elf32_swap_reloc_in,
  bfd_elf32_swap_reloc_out,
  bfd_elf32_swap_reloca_in,
  bfd_elf32_swap_reloca_out
};

#define elf_backend_size_info		arc_elf32_size_info

static struct bfd_link_hash_table *
arc_elf_link_hash_table_create (bfd *abfd)
{
  struct elf_link_hash_table *htab;

  htab = bfd_zmalloc (sizeof (*htab));
  if (htab == NULL)
    return NULL;

  if (!_bfd_elf_link_hash_table_init (htab, abfd,
				      _bfd_elf_link_hash_newfunc,
				      sizeof (struct elf_link_hash_entry),
				      GENERIC_ELF_DATA))
    {
      free (htab);
      return NULL;
    }

  htab->init_got_refcount.refcount = 0;
  htab->init_got_refcount.glist = NULL;
  htab->init_got_offset.offset = 0;
  htab->init_got_offset.glist = NULL;
  return (struct bfd_link_hash_table *) htab;
}

/* Hook called by the linker routine which adds symbols from an object
   file.  */

static bfd_boolean
elf_arc_add_symbol_hook (bfd * abfd,
			 struct bfd_link_info * info,
			 Elf_Internal_Sym * sym,
			 const char ** namep ATTRIBUTE_UNUSED,
			 flagword * flagsp ATTRIBUTE_UNUSED,
			 asection ** secp ATTRIBUTE_UNUSED,
			 bfd_vma * valp ATTRIBUTE_UNUSED)
{
  if ((ELF_ST_TYPE (sym->st_info) == STT_GNU_IFUNC
       || ELF_ST_BIND (sym->st_info) == STB_GNU_UNIQUE)
      && (abfd->flags & DYNAMIC) == 0
      && bfd_get_flavour (info->output_bfd) == bfd_target_elf_flavour)
    elf_tdata (info->output_bfd)->has_gnu_symbols = elf_gnu_symbol_any;

  return TRUE;
}

#define TARGET_LITTLE_SYM   arc_elf32_le_vec
#define TARGET_LITTLE_NAME  "elf32-littlearc"
#define TARGET_BIG_SYM	    arc_elf32_be_vec
#define TARGET_BIG_NAME     "elf32-bigarc"
#define ELF_ARCH	    bfd_arch_arc
#define ELF_MACHINE_CODE    EM_ARC_COMPACT
#define ELF_MACHINE_ALT1    EM_ARC_COMPACT2
#define ELF_MAXPAGESIZE     0x2000

#define bfd_elf32_bfd_link_hash_table_create	arc_elf_link_hash_table_create

#define bfd_elf32_bfd_merge_private_bfd_data    arc_elf_merge_private_bfd_data
#define bfd_elf32_bfd_reloc_type_lookup		arc_elf32_bfd_reloc_type_lookup
#define bfd_elf32_bfd_set_private_flags		arc_elf_set_private_flags
#define bfd_elf32_bfd_print_private_bfd_data    arc_elf_print_private_bfd_data
#define bfd_elf32_bfd_copy_private_bfd_data     arc_elf_copy_private_bfd_data

#define elf_info_to_howto_rel		     arc_info_to_howto_rel
#define elf_backend_object_p		     arc_elf_object_p
#define elf_backend_final_write_processing   arc_elf_final_write_processing

#define elf_backend_relocate_section	     elf_arc_relocate_section
#define elf_backend_check_relocs	     elf_arc_check_relocs
#define elf_backend_create_dynamic_sections  _bfd_elf_create_dynamic_sections

#define elf_backend_adjust_dynamic_symbol    elf_arc_adjust_dynamic_symbol
#define elf_backend_finish_dynamic_symbol    elf_arc_finish_dynamic_symbol

#define elf_backend_finish_dynamic_sections  elf_arc_finish_dynamic_sections
#define elf_backend_size_dynamic_sections    elf_arc_size_dynamic_sections
#define elf_backend_add_symbol_hook          elf_arc_add_symbol_hook

#define elf_backend_can_gc_sections	1
#define elf_backend_want_got_plt	1
#define elf_backend_plt_readonly	1
#define elf_backend_rela_plts_and_copies_p 1
#define elf_backend_want_plt_sym	0
#define elf_backend_got_header_size	12

#define elf_backend_may_use_rel_p	0
#define elf_backend_may_use_rela_p	1
#define elf_backend_default_use_rela_p	1

#include "elf32-target.h"

#ifndef EXT2_STRUCTS_H
#define EXT2_STRUCTS_H

// Ext2 superblock structure, containing filesystem metadata and configuration parameters
struct Superblock {
  unsigned inodes_count;
  unsigned blocks_count;
  unsigned r_blocks_count;
  unsigned free_blocks_count;
  unsigned free_inodes_count;
  unsigned first_data_block;
  unsigned log_block_size;
  unsigned log_frag_size;
  unsigned blocks_per_group;
  unsigned frags_per_group;
  unsigned inodes_per_group;
  unsigned mtime;
  unsigned wtime;
  unsigned short mnt_count;
  unsigned short max_mnt_count;
  unsigned short magic;
  unsigned short state;
  unsigned short errors;
  unsigned short minor_rev_level;
  unsigned lastcheck;
  unsigned checkinterval;
  unsigned creator_os;
  unsigned rev_level;
  unsigned short def_resuid;
  unsigned short def_resgid;
  unsigned first_ino;
  unsigned short inode_size;
  unsigned short block_group_nr;
  unsigned feature_compat;
  unsigned feature_incompat;
  unsigned feature_ro_compat;
  unsigned char uuid[16];
  unsigned char volume_name[16];
  unsigned char last_mounted[64];
  unsigned algo_bitmap;
  unsigned char prealloc_blocks;
  unsigned char prealloc_dir_blocks;
  unsigned char journal_uuid[16];
  unsigned journal_inum;
  unsigned journal_dev;
  unsigned last_orphan;
  unsigned hash_seed[4];
  unsigned char def_hash_version;
  unsigned default_mount_options;
  unsigned first_meta_bg;

  char reserved[760];
};

// superblock constants

// s_state values
#define EXT2_VALID_FS 1
#define EXT2_ERROR_FS 2

// s_errors values
#define EXT2_ERRORS_CONTINUE 1
#define EXT2_ERRORS_RO       2
#define EXT2_ERRORS_PANIC    3

// s_creator_os values
#define EXT2_OS_LINUX    0
#define EXT2_OS_HURD     1
#define EXT2_OS_MASIX    2
#define EXT2_OS_FREEBSD  3
#define EXT2_OS_LITES    4
#define EXT2_OS_DIOPTASE 5

// s_rev_level values
#define EXT2_GOOD_OLD_REV 0
#define EXT2_DYNAMIC_REV  1

// s_feature_compat_values
#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_NO     0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020

// s_feature_incompat values
#define EXT2_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE    0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER     0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG     0x0010

// s_feature_ro_compat
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    0x0004

// s_algo_bitmap 
#define EXT2_LZV1_ALG   0x00000001
#define EXT2_LZRW3A_ALG 0x00000002
#define EXT2_GZIP_ALG   0x00000004
#define EXT2_BZIP2_ALG  0x00000008
#define EXT2_LZO_ALG    0x00000010

// Block group descriptor entry
struct BGD {
  unsigned block_bitmap;
  unsigned inode_bitmap;
  unsigned inode_table;
  unsigned short free_blocks_count;
  unsigned short free_inodes_count;
  unsigned short used_dirs_count;
  unsigned short pad;
  unsigned char  reserved[12];
};

// Ext2 inode structure
// contains metadata about a file or directory and pointers to its data blocks
struct Inode {
  unsigned short mode;
  unsigned short uid;
  unsigned size;
  unsigned atime;
  unsigned ctime;
  unsigned mtime;
  unsigned dtime;
  unsigned short gid;
  unsigned short links_count;
  unsigned blocks;
  unsigned flags;
  unsigned osd1;
  unsigned block[15];
  unsigned generation;
  unsigned file_acl;
  unsigned dir_acl;
  unsigned faddr;
  unsigned char osd2[12];
};

// inode constants:

// reserved inodes
#define EXT2_BAD_INO         1
#define EXT2_ROOT_INO        2
#define EXT2_ACL_IDX_INO     3
#define EXT2_ACL_DATA_INO    4
#define EXT2_BOOT_LOADER_INO 5
#define EXT2_UNDEL_DIR_INO   6

// i_mode values
#define EXT2_S_MASK   0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000
#define EXT2_S_ISUID  0x0800
#define EXT2_S_ISGID  0x0400
#define EXT2_S_ISVTX  0x0200
#define EXT2_S_IRUSR  0x0100
#define EXT2_S_IWUSR  0x0080
#define EXT2_S_IXUSR  0x0040
#define EXT2_S_IRGRP  0x0020
#define EXT2_S_IWGRP  0x0010
#define EXT2_S_IXGRP  0x0008
#define EXT2_S_IROTH  0x0004
#define EXT2_S_IWOTH  0x0002
#define EXT2_S_IXOTH  0x0001

// i_flags values
#define EXT2_SECRM_FL        0x00000001
#define EXT2_UNRM_FL         0x00000002
#define EXT2_COMPR_FL        0x00000004
#define EXT2_SYNC_FL         0x00000008
#define EXT2_IMMUTABLE_FL    0x00000010
#define EXT2_APPEND_FL       0x00000020
#define EXT2_NODUMP_FL       0x00000040
#define EXT2_NOATIME_FL      0x00000080
#define EXT2_DIRTY_FL        0x00000100
#define EXT2_COMPRBLK_FL     0x00000200
#define EXT2_NOCOMPR_FL      0x00000400
#define EXT2_ECOMPR_FL       0x00000800
#define EXT2_BTREE_FL        0x00001000
#define EXT2_INDEX_FL        0x00001000
#define EXT2_IMAGIC_FL       0x00002000
#define EXT2_JOURNAL_DATA_FL 0x00004000
#define EXT2_RESERVED_FL     0x80000000

// Directory entry structure, used for entries in directory data blocks
struct DirEntry {
  unsigned inode;
  unsigned short rec_len;
  unsigned short name_len;
  unsigned char  name[256];
};

// d_type constants.
#define EXT2_DT_UNKNOWN 0
#define EXT2_DT_DIR     4
#define EXT2_DT_REG     8
#define EXT2_DT_LNK     10

struct linux_dirent {
    unsigned d_ino; // i-node number.
    unsigned d_off;
    unsigned short d_reclen; // Length of this record.
    char d_name; // Filename (null-terminated array).
    // char pad;
    // char d_type; // Offset is (d_reclen - 1).
};

#endif // EXT2_STRUCTS_H

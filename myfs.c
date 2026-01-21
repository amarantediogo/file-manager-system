/*
 *  myfs.c - Implementacao do sistema de arquivos MyFS
 *
 *  Autores: SUPER_PROGRAMADORES_C
 *  Projeto: Trabalho Pratico II - Sistemas Operacionais
 *  Organizacao: Universidade Federal de Juiz de Fora
 *  Departamento: Dep. Ciencia da Computacao
 *
 */

#include "myfs.h"
#include "disk.h"
#include "inode.h"
#include "util.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// DECLARACOES GLOBAIS
#define SUPERBLOCK_SECTOR 0
#define ROOT_INODE 1

// Estrutura para representar um descritor de arquivo aberto
typedef struct {
  int used;
  unsigned int inumber;
  unsigned int cursor;
  Disk *disk;
} FileDescriptor;

typedef struct {
  unsigned long int nextClusterAddress;
} FreeClusterHeader;

typedef struct {
  unsigned int numInodes;
  unsigned int blockSize;
  unsigned long int dataBeginSector;
  unsigned long int dataLastCluster;
  unsigned long int firstFreeClusterAddress;
} SuperBlock;

typedef struct {
  unsigned int inodeNumber;
  char name[MAX_FILENAME_LENGTH + 1];
} DirEntry;

static FileDescriptor openFiles[MAX_FDS];
static int initialized = 0;
static int myfsMounted = 0;
static unsigned int sbBlockSize = 0;
static unsigned int sbFirstDataSector = 0;
static unsigned int sbNumInodes = 0;
static unsigned int sbTotalBlocks = 0;

// FUNCOES AUXILIARES

static void initFileDescriptors() {
  if (!initialized) {
    for (int i = 0; i < MAX_FDS; i++) {
      openFiles[i].used = 0;
      openFiles[i].inumber = 0;
      openFiles[i].cursor = 0;
      openFiles[i].disk = NULL;
    }
    initialized = 1;
  }
}

static int writeSuperBlock(Disk *d, const SuperBlock *sb) {
  unsigned char sector[DISK_SECTORDATASIZE];
  memset(sector, 0, sizeof(sector));

  memcpy(sector, "MYFS", 4);
  memcpy(&sector[4], sb, sizeof(SuperBlock));

  if (diskWriteSector(d, SUPERBLOCK_SECTOR, sector) != 0)
    return -1;

  return 0;
}

static int writeBlock(Disk *d, unsigned long int firstSector,
                      unsigned int blockSize, const unsigned char *in) {
  unsigned int sectorsPerBlock = blockSize / DISK_SECTORDATASIZE;

  for (unsigned int i = 0; i < sectorsPerBlock; i++) {
    if (diskWriteSector(d, firstSector + i,
                        (unsigned char *)(in + (i * DISK_SECTORDATASIZE))) != 0)
      return -1;
  }
  return 0;
}

static int readBlock(Disk *d, unsigned long int firstSector,
                     unsigned int blockSize, unsigned char *out) {
  unsigned int sectorsPerBlock = blockSize / DISK_SECTORDATASIZE;

  for (unsigned int i = 0; i < sectorsPerBlock; i++) {
    if (diskReadSector(d, firstSector + i, out + (i * DISK_SECTORDATASIZE)) !=
        0)
      return -1;
  }
  return 0;
}

static int readSuperBlock(Disk *d, SuperBlock *sb) {
  unsigned char sector[DISK_SECTORDATASIZE];
  if (diskReadSector(d, SUPERBLOCK_SECTOR, sector) != 0)
    return -1;

  if (sector[0] != 'M' || sector[1] != 'Y' || sector[2] != 'F' ||
      sector[3] != 'S')
    return -1;

  // Formatação escreveu: "MYFS" + struct SuperBlock a partir do offset 4
  memcpy(sb, &sector[4], sizeof(SuperBlock));
  return 0;
}

// Aloca 1 bloco (cluster) usando a free list encadeada no início de cada
// cluster. Retorna o endereço (setor inicial do cluster) ou 0 em erro.
static unsigned long int allocateFreeCluster(Disk *d, SuperBlock *sb) {
  if (sb->firstFreeClusterAddress == 0)
    return 0;

  unsigned long int allocated = sb->firstFreeClusterAddress;

  // Lê o header do cluster livre para descobrir o próximo livre
  unsigned char sector0[DISK_SECTORDATASIZE];
  if (diskReadSector(d, allocated, sector0) != 0)
    return 0;

  FreeClusterHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  memcpy(&hdr.nextClusterAddress, sector0, sizeof(unsigned long int));

  // Atualiza cabeça da free list
  sb->firstFreeClusterAddress = hdr.nextClusterAddress;

  // Persistir superbloco com a nova cabeça
  if (writeSuperBlock(d, sb) != 0)
    return 0;

  // (Opcional) “limpar” o primeiro setor do cluster alocado
  memset(sector0, 0, sizeof(sector0));
  if (diskWriteSector(d, allocated, sector0) != 0)
    return 0;

  return allocated;
}

// Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
// se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
// um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle(Disk *d) {
  initFileDescriptors();
  for (int i = 0; i < MAX_FDS; i++) {
    if (openFiles[i].used) {
      return 0;
    }
  }
  return 1;
}

// Funcao para formatacao de um disco com o novo sistema de arquivos
// com tamanho de blocos igual a blockSize. Retorna o numero total de
// blocos disponiveis no disco, se formatado com sucesso. Caso contrario,
// retorna -1.
int myFSFormat(Disk *d, unsigned int blockSize) {
  printf("\n-- Formatting disk %d...", diskGetId(d));
  printf("\n   Block size: %u bytes", blockSize);
  printf("\n   Disk size: %lu bytes", diskGetSize(d));
  SLEEP(1000);

  // ========== VALIDACAO DE PARAMETROS ==========
  if (!d) {
    printf("\n!! Error: Invalid disk pointer (NULL). Disk ID: %d\n",
           diskGetId(d));
    return -1;
  }

  if (blockSize == 0) {
    printf("\n!! Error: Block size cannot be zero. Disk ID: %d\n",
           diskGetId(d));
    return -1;
  }

  if ((blockSize % DISK_SECTORDATASIZE) != 0) {
    printf("\n!! Error: Block size (%u) must be multiple of sector size (%d). "
           "Disk ID: %d\n",
           blockSize, DISK_SECTORDATASIZE, diskGetId(d));
    return -1;
  }

  // ========== CALCULO DE LAYOUT DO DISCO ==========
  unsigned long totalSectors = diskGetNumSectors(d);
  unsigned long diskSize = diskGetSize(d);

  // Calcula quantos inodes serão criados no disco
  // Usa uma proporção razoável: 1 inode a cada 8 blocos de dados
  unsigned int blocksInDisk = totalSectors / (blockSize / DISK_SECTORDATASIZE);
  unsigned int numInodes = blocksInDisk / 8;
  if (numInodes < 8) {
    numInodes = 8;
  }

  unsigned int maxInodes = 1024;
  if (numInodes > maxInodes) {
    numInodes = maxInodes;
  }

  if (numInodes < 1) {
    printf("\n!! Error: Disk too small. Cannot fit any inodes. Disk ID: %d\n",
           diskGetId(d));
    return -1;
  }

  // Calcula onde começa a area de dados (após superbloco e inodes)
  unsigned int inodesBeginSector = inodeAreaBeginSector();
  unsigned int inodesSectors =
      (numInodes + inodeNumInodesPerSector() - 1) / inodeNumInodesPerSector();
  unsigned int dataBeginSector = inodesBeginSector + inodesSectors;

  // Alinha dataBeginSector com o tamanho do cluster
  unsigned int sectorsPerCluster = blockSize / DISK_SECTORDATASIZE;
  unsigned int misalignment = dataBeginSector % sectorsPerCluster;
  if (misalignment != 0) {
    dataBeginSector += sectorsPerCluster - misalignment;
  }

  // Valida se há espaço suficiente para dados
  if (dataBeginSector >= totalSectors) {
    printf("\n!! Error: No space for data after metadata. Disk ID: %d\n",
           diskGetId(d));
    printf("   Total sectors: %lu, Data would start at: %u\n", totalSectors,
           dataBeginSector);
    return -1;
  }

  // Calcula clusters disponíveis
  unsigned long dataSectors = totalSectors - dataBeginSector;
  unsigned int totalClusters = dataSectors / sectorsPerCluster;

  if (totalClusters < 2) {
    printf("\n!! Error: Insufficient space for data clusters. Disk ID: %d\n",
           diskGetId(d));
    printf("   Data sectors available: %lu, Clusters: %u\n", dataSectors,
           totalClusters);
    return -1;
  }

  printf("\n   Layout calculated:");
  printf("\n   - Inodes: %d (sectors %u to %u)", numInodes, inodesBeginSector,
         dataBeginSector - 1);
  printf("\n   - Data: %u clusters (%lu sectors)\n", totalClusters,
         dataSectors);

  // ========== INICIALIZACAO DE SETORES DE METADATA ==========
  printf("\n-- Initializing metadata sectors...");
  for (unsigned long i = 0; i < dataBeginSector; i++) {
    unsigned char emptySector[DISK_SECTORDATASIZE] = {0};
    if (diskWriteSector(d, i, emptySector) != 0) {
      printf("\n!! Error: Failed to write metadata sector %lu. Disk ID: %d\n",
             i, diskGetId(d));
      return -1;
    }
  }

  // ========== INICIALIZACAO DE SETORES DE DADOS (FREE LIST) ==========
  printf("\n-- Initializing data sectors and free list...");
  for (unsigned long i = dataBeginSector; i < totalSectors; i++) {
    unsigned char emptySector[DISK_SECTORDATASIZE] = {0};

    // No início de cada cluster, cria header da lista de clusters livres
    if ((i - dataBeginSector) % sectorsPerCluster == 0) {
      FreeClusterHeader header;
      header.nextClusterAddress = i + sectorsPerCluster;
      memcpy(emptySector, &header.nextClusterAddress,
             sizeof(unsigned long int));
    }

    if (diskWriteSector(d, i, emptySector) != 0) {
      printf("\n!! Error: Failed to write data sector %lu. Disk ID: %d\n", i,
             diskGetId(d));
      return -1;
    }
  }

  // ========== CRIACAO E GRAVACAO DO SUPERBLOCO ==========
  printf("\n-- Writing superblock...");
  SuperBlock superblock;
  superblock.numInodes = numInodes;
  superblock.blockSize = blockSize;
  superblock.dataBeginSector = dataBeginSector;
  superblock.dataLastCluster = totalClusters - 1;
  superblock.firstFreeClusterAddress = dataBeginSector;

  unsigned char superblockData[DISK_SECTORDATASIZE] = {0};
  memcpy(superblockData, "MYFS", 4);
  memcpy(&superblockData[4], &superblock, sizeof(SuperBlock));

  if (diskWriteSector(d, SUPERBLOCK_SECTOR, superblockData) != 0) {
    printf("\n!! Error: Failed to write superblock. Disk ID: %d\n",
           diskGetId(d));
    return -1;
  }

  // ========== CRIACAO DE INODES VAZIOS ==========
  printf("\n-- Creating %d empty inodes...", numInodes);
  for (unsigned int i = 0; i < numInodes; i++) {
    Inode *inode = inodeCreate(i + 1, d);
    if (inode == NULL) {
      printf("\n!! Error: Failed to create inode %u. Disk ID: %d\n", i + 1,
             diskGetId(d));
      return -1;
    }
    free(inode);
  }

  // ========== CRIACAO DO DIRETORIO ROOT ==========
  printf("\n-- Creating root directory...");

  // Carrega inode do ROOT
  Inode *rootInode = inodeLoad(ROOT_INODE, d);
  if (rootInode == NULL) {
    printf("\n!! Error: Failed to load root inode. Disk ID: %d\n",
           diskGetId(d));
    return -1;
  }

  // Configura atributos do ROOT (sem subdiretórios)
  inodeSetFileType(rootInode, FILETYPE_DIR);
  inodeSetOwner(rootInode, 0);
  inodeSetGroupOwner(rootInode, 0);
  inodeSetPermission(rootInode, 0);
  inodeSetRefCount(rootInode, 1); // Apenas o diretório root em si

  if (inodeSave(rootInode) != 0) {
    printf("\n!! Error: Failed to save root inode. Disk ID: %d\n",
           diskGetId(d));
    free(rootInode);
    return -1;
  }

  free(rootInode);

  // ========== CALCULO DE BLOCOS DISPONIVEIS ==========
  // Todos os clusters estão disponíveis (root não ocupa clusters de dados)
  unsigned long availableClusters = superblock.dataLastCluster;

  if (availableClusters == 0) {
    printf("\n!! No blocks available after formatting. Disk ID: %d\n",
           diskGetId(d));
    printf("   Total clusters: %lu\n", superblock.dataLastCluster);
    return -1;
  }

  printf("\n-- Formatting completed successfully!");
  printf("\n   Available data blocks: %lu\n", availableClusters);

  return (int)availableClusters;
}

// Funcao para montagem/desmontagem do sistema de arquivos, se possível.
// Na montagem (x=1) e' a chance de se fazer inicializacoes, como carregar
// o superbloco na memoria. Na desmontagem (x=0), quaisquer dados pendentes
// de gravacao devem ser persistidos no disco. Retorna um positivo se a
// montagem ou desmontagem foi bem sucedida ou, caso contrario, 0.
int myFSxMount(Disk *d, int x) {
  if (!d)
    return 0;
  if (x == 1) {
    unsigned char sector[DISK_SECTORDATASIZE];
    if (diskReadSector(d, 0, sector) != 0)
      return 0;
    if (sector[0] != 'M' || sector[1] != 'Y' || sector[2] != 'F' ||
        sector[3] != 'S')
      return 0;
    unsigned int bs = 0, nin = 0, fds = 0, tb = 0;
    char2ul(&sector[4], &bs);
    char2ul(&sector[8], &nin);
    char2ul(&sector[12], &fds);
    char2ul(&sector[16], &tb);
    if (bs == 0 || bs > DISK_SECTORDATASIZE || (DISK_SECTORDATASIZE % bs) != 0)
      return 0;
    unsigned long ns = diskGetNumSectors(d);
    if (fds >= ns)
      return 0;
    sbBlockSize = bs;
    sbNumInodes = nin;
    sbFirstDataSector = fds;
    sbTotalBlocks = tb;
    myfsMounted = 1;
    initFileDescriptors();
    return 1;
  } else if (x == 0) {
    if (!myfsMounted)
      return 0;
    myfsMounted = 0;
    sbBlockSize = sbNumInodes = sbFirstDataSector = sbTotalBlocks = 0;
    return 1;
  }
  return 0;
}

// Funcao para abertura de um arquivo, a partir do caminho especificado
// em path, no disco montado especificado em d, no modo Read/Write,
// criando o arquivo se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen(Disk *d, const char *path) {
  if (!d || !path)
    return -1;

  if (!myfsMounted)
    return -1;

  initFileDescriptors();

  if (strlen(path) == 0)
    return -1;

  unsigned int inumber = inodeFindFreeInode(ROOT_INODE + 1, d);
  if (inumber == 0)
    return -1;

  Inode *fileInode = inodeCreate(inumber, d);
  if (!fileInode)
    return -1;

  inodeSetFileType(fileInode, FILETYPE_REGULAR);
  inodeSetFileSize(fileInode, 0);
  inodeSetRefCount(fileInode, 1);

  if (inodeSave(fileInode) < 0) {
    free(fileInode);
    return -1;
  }

  free(fileInode);

  for (int i = 0; i < MAX_FDS; i++) {
    if (!openFiles[i].used) {
      openFiles[i].used = 1;
      openFiles[i].inumber = inumber;
      openFiles[i].cursor = 0;
      openFiles[i].disk = d;
      return i + 1;
    }
  }

  return -1;
}

// Funcao para a leitura de um arquivo, a partir de um descritor de arquivo
// existente. Os dados devem ser lidos a partir da posicao atual do cursor
// e copiados para buf. Terao tamanho maximo de nbytes. Ao fim, o cursor
// deve ter posicao atualizada para que a proxima operacao ocorra a partir
// do próximo byte apos o ultimo lido. Retorna o numero de bytes
// efetivamente lidos em caso de sucesso ou -1, caso contrario.
int myFSRead(int fd, char *buf, unsigned int nbytes)
{
  if (!myfsMounted)
    return -1;

  initFileDescriptors();

  if (fd <= 0 || fd > MAX_FDS || buf == NULL)
    return -1;

  int idx = fd - 1;
  if (!openFiles[idx].used)
    return -1;

  if (nbytes == 0)
    return 0;

  Disk *d = openFiles[idx].disk;
  if (!d) return -1;

  unsigned int inumber = openFiles[idx].inumber;

  Inode *inode = inodeLoad(inumber, d);
  if (!inode)
    return -1;

  SuperBlock sb;
  if (readSuperBlock(d, &sb) != 0)
  {
    free(inode);
    return -1;
  }

  unsigned int fileSize = inodeGetFileSize(inode);
  // unsigned int cursor = openFiles[idx].cursor;
  unsigned int cursor = 0;

  if (cursor >= fileSize)
  {
    free(inode);
    return 0; // EOF
  }

  unsigned int canRead = fileSize - cursor;
  unsigned int toRead = (nbytes < canRead) ? nbytes : canRead;

  unsigned int blockSize = sb.blockSize;
  unsigned int readBytes = 0;

  unsigned char *blockBuf = (unsigned char *)malloc(blockSize);
  if (!blockBuf)
  {
    free(inode);
    return -1;
  }

  while (readBytes < toRead)
  {
    unsigned int pos = cursor + readBytes;
    unsigned int blockIndex = pos / blockSize;
    unsigned int offInBlock = pos % blockSize;

    unsigned long int blockAddr = inodeGetBlockAddr(inode, blockIndex);
    if (blockAddr == 0)
    {
      free(blockBuf);
      free(inode);
      return -1;
    }

    if (readBlock(d, blockAddr, blockSize, blockBuf) != 0)
    {
      free(blockBuf);
      free(inode);
      return -1;
    }

    unsigned int remaining = toRead - readBytes;
    unsigned int chunk = blockSize - offInBlock;
    if (chunk > remaining)
      chunk = remaining;

    memcpy(buf + readBytes, blockBuf + offInBlock, chunk);
    readBytes += chunk;
  }

  free(blockBuf);

  free(inode);
  return (int)readBytes;
}

// Funcao para a escrita de um arquivo, a partir de um descritor de arquivo
// existente. Os dados de buf sao copiados para o disco a partir da posição
// atual do cursor e terao tamanho maximo de nbytes. Ao fim, o cursor deve
// ter posicao atualizada para que a proxima operacao ocorra a partir do
// proximo byte apos o ultimo escrito. Retorna o numero de bytes
// efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite(int fd, const char *buf, unsigned int nbytes) {
  if (!myfsMounted)
    return -1;

  initFileDescriptors();

  if (fd <= 0 || fd > MAX_FDS || buf == NULL)
    return -1;

  int idx = fd - 1;
  if (!openFiles[idx].used)
    return -1;

  if (nbytes == 0)
    return 0;

  Disk *d = openFiles[idx].disk;
  if (!d)
    return -1;
  unsigned int inumber = openFiles[idx].inumber;

  Inode *inode = inodeLoad(inumber, d);
  if (!inode)
    return -1;

  SuperBlock sb;
  if (readSuperBlock(d, &sb) != 0) {
    free(inode);
    return -1;
  }

  unsigned int blockSize = sb.blockSize;
  unsigned int cursor = openFiles[idx].cursor;
  unsigned int fileSize = inodeGetFileSize(inode);

  unsigned int written = 0;

  unsigned char *blockBuf = (unsigned char *)malloc(blockSize);
  if (!blockBuf) {
    free(inode);
    return -1;
  }

  while (written < nbytes) {
    unsigned int pos = cursor + written;
    unsigned int blockIndex = pos / blockSize;
    unsigned int offInBlock = pos % blockSize;

    // Garantir que existe bloco para esse blockIndex:
    // número de blocos atuais (arredonda pra cima)
    unsigned int blocksNow = (fileSize + blockSize - 1) / blockSize;

    while (blockIndex >= blocksNow) {
      unsigned long int newBlockAddr = allocateFreeCluster(d, &sb);
      if (newBlockAddr == 0) {
        free(blockBuf);
        free(inode);
        return -1;
      }

      if (inodeAddBlock(inode, newBlockAddr) != 0) {
        free(blockBuf);
        free(inode);
        return -1;
      }

      blocksNow++;
    }

    unsigned long int blockAddr = inodeGetBlockAddr(inode, blockIndex);
    if (blockAddr == 0) {
      free(blockBuf);
      free(inode);
      return -1;
    }

    // Se escrita não cobre o bloco inteiro, fazemos read-modify-write
    unsigned int remaining = nbytes - written;
    unsigned int chunk = blockSize - offInBlock;
    if (chunk > remaining)
      chunk = remaining;

    if (offInBlock != 0 || chunk != blockSize) {
      if (readBlock(d, blockAddr, blockSize, blockBuf) != 0) {
        free(blockBuf);
        free(inode);
        return -1;
      }
    } else {
      memset(blockBuf, 0, blockSize);
    }

    memcpy(blockBuf + offInBlock, buf + written, chunk);

    if (writeBlock(d, blockAddr, blockSize, blockBuf) != 0) {
      free(blockBuf);
      free(inode);
      return -1;
    }

    written += chunk;

    // Se ultrapassou o tamanho, atualiza
    unsigned int endPos = cursor + written;
    if (endPos > fileSize)
      fileSize = endPos;
  }

  free(blockBuf);

  // Atualiza cursor e tamanho do arquivo no inode
  openFiles[idx].cursor += written;
  inodeSetFileSize(inode, fileSize);

  if (inodeSave(inode) != 0) {
    free(inode);
    return -1;
  }

  free(inode);
  return (int)written;
}

// Funcao para fechar um arquivo, a partir de um descritor de arquivo
// existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose(int fd) {
  // Verifica se o descritor é válido (FDs começam em 1)
  if (fd <= 0 || fd > MAX_FDS)
    return -1;

  int index = fd - 1;

  // Verifica se o arquivo realmente está aberto
  if (!openFiles[index].used)
    return -1;

  // Zera o cursor
  openFiles[index].cursor = 0;

  // Libera o descritor
  openFiles[index].used = 0;
  openFiles[index].inumber = 0;

  return 0;
}

// Funcao para abertura de um diretorio, a partir do caminho
// especificado em path, no disco indicado por d, no modo Read/Write,
// criando o diretorio se nao existir. Retorna um descritor de arquivo,
// em caso de sucesso. Retorna -1, caso contrario.
int myFSOpenDir(Disk *d, const char *path) { return -1; }

// Funcao para a leitura de um diretorio, identificado por um descritor
// de arquivo existente. Os dados lidos correspondem a uma entrada de
// diretorio na posicao atual do cursor no diretorio. O nome da entrada
// e' copiado para filename, como uma string terminada em \0 (max 255+1).
// O numero do inode correspondente 'a entrada e' copiado para inumber.
// Retorna 1 se uma entrada foi lida, 0 se fim de diretorio ou -1 caso
// mal sucedido
int myFSReadDir(int fd, char *filename, unsigned int *inumber) { return -1; }

// Funcao para adicionar uma entrada a um diretorio, identificado por um
// descritor de arquivo existente. A nova entrada tera' o nome indicado
// por filename e apontara' para o numero de i-node indicado por inumber.
// Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSLink(int fd, const char *filename, unsigned int inumber) { return -1; }

// Funcao para remover uma entrada existente em um diretorio,
// identificado por um descritor de arquivo existente. A entrada e'
// identificada pelo nome indicado em filename. Retorna 0 caso bem
// sucedido, ou -1 caso contrario.
int myFSUnlink(int fd, const char *filename) { return -1; }

// Funcao para fechar um diretorio, identificado por um descritor de
// arquivo existente. Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSCloseDir(int fd) { return -1; }

// Funcao para instalar seu sistema de arquivos no S.O., registrando-o junto
// ao virtual FS (vfs). Retorna um identificador unico (slot), caso
// o sistema de arquivos tenha sido registrado com sucesso.
// Caso contrario, retorna -1
int installMyFS(void) {
  static FSInfo fs;              // Persistente
  static char fsname[] = "myfs"; // Persistente

  // Zerar toda a estrutura
  memset(&fs, 0, sizeof(FSInfo));

  // DEFINIÇÃO DO FSID
  fs.fsid = 0;
  fs.fsname = fsname;

  // Funções basicas
  fs.isidleFn = myFSIsIdle;
  fs.formatFn = myFSFormat;
  fs.xMountFn = myFSxMount;

  // Arquivos
  fs.openFn = myFSOpen;
  fs.readFn = myFSRead;
  fs.writeFn = myFSWrite;
  fs.closeFn = myFSClose;

  if (vfsRegisterFS(&fs) < 0) {
    printf("Falha ao registrar o MyFS no VFS.\n");
    return -1;
  }

  printf("MyFS registrado com sucesso (fsid = %d).\n", fs.fsid);
  return fs.fsid;
}

/*
*  myfs.c - Implementacao do sistema de arquivos MyFS
*
*  Autores: SUPER_PROGRAMADORES_C
*  Projeto: Trabalho Pratico II - Sistemas Operacionais
*  Organizacao: Universidade Federal de Juiz de Fora
*  Departamento: Dep. Ciencia da Computacao
*
*/

#include <stdio.h>
#include <stdlib.h>
#include "myfs.h"
#include "vfs.h"
#include "inode.h"
#include "util.h"

//Declaracoes globais

// Estrutura para representar um descritor de arquivo aberto
typedef struct
{
	int used;
	unsigned int inumber;
	unsigned int cursor;
} FileDescriptor;

static FileDescriptor openFiles[MAX_FDS];
static int initialized = 0;
static int myfsMounted = 0;
static unsigned int sbBlockSize = 0;
static unsigned int sbNumInodes = 0;
static unsigned int sbFirstDataSector = 0;
static unsigned int sbTotalBlocks = 0;

// Funcoes auxiliares

// Funcao para inicializar a tabela de descritores de arquivos
static void initFileDescriptors() {
    if (!initialized) {
        for (int i = 0; i < MAX_FDS; i++) {
            openFiles[i].used = 0;
            openFiles[i].inumber = 0;
            openFiles[i].cursor = 0;
        }
        initialized = 1;
    }
}

//Funcao para verificacao se o sistema de arquivos está ocioso, ou seja,
//se nao ha quisquer descritores de arquivos em uso atualmente. Retorna
//um positivo se ocioso ou, caso contrario, 0.
int myFSIsIdle (Disk *d) {
    initFileDescriptors();
    for (int i = 0; i < MAX_FDS; i++) {
        if (openFiles[i].used) {
            return 0;
        }
    }
    return 1;
}

//Funcao para formatacao de um disco com o novo sistema de arquivos
//com tamanho de blocos igual a blockSize. Retorna o numero total de
//blocos disponiveis no disco, se formatado com sucesso. Caso contrario,
//retorna -1.
int myFSFormat (Disk *d, unsigned int blockSize) {
	if (!d) return -1;
	// Aceitamos apenas blockSize que caiba inteiro em um setor
	if (blockSize == 0 || blockSize > DISK_SECTORDATASIZE ||
	    (DISK_SECTORDATASIZE % blockSize) != 0) {
		return -1;
	}

	unsigned long numSectors = diskGetNumSectors(d);
	unsigned int inodesPerSector = inodeNumInodesPerSector();
	unsigned int numInodes = 256;
	unsigned int numInodeSectors = (numInodes + inodesPerSector - 1) / inodesPerSector;
	unsigned long firstDataSector = inodeAreaBeginSector() + numInodeSectors;

	// Checar se ha espaco util
	if (numSectors <= firstDataSector) return -1;

	unsigned char superblock[DISK_SECTORDATASIZE];
	unsigned char zeroBuf[DISK_SECTORDATASIZE];
	for (int i = 0; i < DISK_SECTORDATASIZE; i++) {
		superblock[i] = 0;
		zeroBuf[i] = 0;
	}

	// Superbloco: assinatura e metadados basicos
	superblock[0] = 'M'; superblock[1] = 'Y'; superblock[2] = 'F'; superblock[3] = 'S';
	ul2char(blockSize, &superblock[4]);
	ul2char(numInodes, &superblock[8]);
	ul2char(firstDataSector, &superblock[12]);

	if (diskWriteSector(d, 0, superblock) != 0) return -1;

	// Setor 1 reservado/bitmap: zera
	if (diskWriteSector(d, 1, zeroBuf) != 0) return -1;

	// Zerar area de i-nodes
	for (unsigned int s = 0; s < numInodeSectors; s++) {
		if (diskWriteSector(d, inodeAreaBeginSector() + s, zeroBuf) != 0) return -1;
	}

	// Criar i-node raiz (numero 1)
	Inode *rootInode = inodeCreate(1, d);
	if (!rootInode) return -1;

	inodeSetFileType(rootInode, FILETYPE_DIR);
	inodeSetFileSize(rootInode, 0);
	inodeSetOwner(rootInode, 0);
	inodeSetGroupOwner(rootInode, 0);
	inodeSetPermission(rootInode, 0777);
	inodeSetRefCount(rootInode, 1);

	if (inodeSave(rootInode) != 0) {
		free(rootInode);
		return -1;
	}
	free(rootInode);

	// Calcular blocos disponiveis
	unsigned long availableSectors = numSectors - firstDataSector;
	unsigned long blocksPerSector = DISK_SECTORDATASIZE / blockSize;
	unsigned long totalBlocks = availableSectors * blocksPerSector;
	if (totalBlocks == 0) return -1;

	// Persistir totalBlocks no superbloco
	ul2char((unsigned int)totalBlocks, &superblock[16]);
	if (diskWriteSector(d, 0, superblock) != 0) return -1;

	return (int)totalBlocks;
}

//Funcao para montagem/desmontagem do sistema de arquivos, se possível.
//Na montagem (x=1) e' a chance de se fazer inicializacoes, como carregar
//o superbloco na memoria. Na desmontagem (x=0), quaisquer dados pendentes
//de gravacao devem ser persistidos no disco. Retorna um positivo se a
//montagem ou desmontagem foi bem sucedida ou, caso contrario, 0.
int myFSxMount (Disk *d, int x) {
	if (!d) return 0;
	if (x == 1) {
		unsigned char sector[DISK_SECTORDATASIZE];
		if (diskReadSector(d, 0, sector) != 0) return 0;
		if (sector[0] != 'M' || sector[1] != 'Y' || sector[2] != 'F' || sector[3] != 'S') return 0;
		unsigned int bs = 0, nin = 0, fds = 0, tb = 0;
		char2ul(&sector[4], &bs);
		char2ul(&sector[8], &nin);
		char2ul(&sector[12], &fds);
		char2ul(&sector[16], &tb);
		if (bs == 0 || bs > DISK_SECTORDATASIZE || (DISK_SECTORDATASIZE % bs) != 0) return 0;
		unsigned long ns = diskGetNumSectors(d);
		if (fds >= ns) return 0;
		sbBlockSize = bs;
		sbNumInodes = nin;
		sbFirstDataSector = fds;
		sbTotalBlocks = tb;
		myfsMounted = 1;
		initFileDescriptors();
		return 1;
	}
	else if (x == 0) {
		if (!myfsMounted) return 0;
		myfsMounted = 0;
		sbBlockSize = sbNumInodes = sbFirstDataSector = sbTotalBlocks = 0;
		return 1;
	}
	return 0;
}

//Funcao para abertura de um arquivo, a partir do caminho especificado
//em path, no disco montado especificado em d, no modo Read/Write,
//criando o arquivo se nao existir. Retorna um descritor de arquivo,
//em caso de sucesso. Retorna -1, caso contrario.
int myFSOpen (Disk *d, const char *path) {
	return -1;
}
	
//Funcao para a leitura de um arquivo, a partir de um descritor de arquivo
//existente. Os dados devem ser lidos a partir da posicao atual do cursor
//e copiados para buf. Terao tamanho maximo de nbytes. Ao fim, o cursor
//deve ter posicao atualizada para que a proxima operacao ocorra a partir
//do próximo byte apos o ultimo lido. Retorna o numero de bytes
//efetivamente lidos em caso de sucesso ou -1, caso contrario.
int myFSRead (int fd, char *buf, unsigned int nbytes) {
	return -1;
}

//Funcao para a escrita de um arquivo, a partir de um descritor de arquivo
//existente. Os dados de buf sao copiados para o disco a partir da posição
//atual do cursor e terao tamanho maximo de nbytes. Ao fim, o cursor deve
//ter posicao atualizada para que a proxima operacao ocorra a partir do
//proximo byte apos o ultimo escrito. Retorna o numero de bytes
//efetivamente escritos em caso de sucesso ou -1, caso contrario
int myFSWrite (int fd, const char *buf, unsigned int nbytes) {
	return -1;
}

//Funcao para fechar um arquivo, a partir de um descritor de arquivo
//existente. Retorna 0 caso bem sucedido, ou -1 caso contrario
int myFSClose (int fd) {
	return -1;
}

//Funcao para abertura de um diretorio, a partir do caminho
//especificado em path, no disco indicado por d, no modo Read/Write,
//criando o diretorio se nao existir. Retorna um descritor de arquivo,
//em caso de sucesso. Retorna -1, caso contrario.
int myFSOpenDir (Disk *d, const char *path) {
	return -1;
}

//Funcao para a leitura de um diretorio, identificado por um descritor
//de arquivo existente. Os dados lidos correspondem a uma entrada de
//diretorio na posicao atual do cursor no diretorio. O nome da entrada
//e' copiado para filename, como uma string terminada em \0 (max 255+1).
//O numero do inode correspondente 'a entrada e' copiado para inumber.
//Retorna 1 se uma entrada foi lida, 0 se fim de diretorio ou -1 caso
//mal sucedido
int myFSReadDir (int fd, char *filename, unsigned int *inumber) {
	return -1;
}

//Funcao para adicionar uma entrada a um diretorio, identificado por um
//descritor de arquivo existente. A nova entrada tera' o nome indicado
//por filename e apontara' para o numero de i-node indicado por inumber.
//Retorna 0 caso bem sucedido, ou -1 caso contrario.
int myFSLink (int fd, const char *filename, unsigned int inumber) {
	return -1;
}

//Funcao para remover uma entrada existente em um diretorio, 
//identificado por um descritor de arquivo existente. A entrada e'
//identificada pelo nome indicado em filename. Retorna 0 caso bem
//sucedido, ou -1 caso contrario.
int myFSUnlink (int fd, const char *filename) {
	return -1;
}

//Funcao para fechar um diretorio, identificado por um descritor de
//arquivo existente. Retorna 0 caso bem sucedido, ou -1 caso contrario.	
int myFSCloseDir (int fd) {
	return -1;
}

//Funcao para instalar seu sistema de arquivos no S.O., registrando-o junto
//ao virtual FS (vfs). Retorna um identificador unico (slot), caso
//o sistema de arquivos tenha sido registrado com sucesso.
//Caso contrario, retorna -1
int installMyFS (void) {
	return -1;
}

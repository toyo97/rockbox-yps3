#include <stdbool.h>
extern bool nand_ll_use_dma;
extern bool nand_ll_write_enable;
void nand_ll_init(void);
int nand_ll_reset(int bank);
int nand_ll_read_raw(int bank, unsigned int page, unsigned int col,
                     unsigned char *buf, unsigned int len);
int nand_ll_ecc_correct(void *data, const unsigned char *ecc, bool sector);
int nand_ll_ecc_encode(const void *data, unsigned char *ecc, int size_sel);
int nand_ll_program_page(int bank, unsigned int page, const unsigned char *data,
                         const unsigned char *spare, unsigned int spare_len);
int nand_ll_erase_block(int bank, unsigned int page);
void nand_set_active(void);

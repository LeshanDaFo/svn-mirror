/* comalramrom.h - Cartridge handling, COMAL RAM/ROM cart */
#ifndef VICE_COMALRAMROM_H
#define VICE_COMALRAMROM_H

#include <stdio.h>
#include <stdint.h>

struct snapshot_s;

/* Lifecycle / Config */
void comalramrom_config_init(void);
void comalramrom_config_setup(uint8_t *rawcart);
int  comalramrom_bin_attach(const char *filename, uint8_t *rawcart);
int  comalramrom_crt_attach(FILE *fd, uint8_t *rawcart);
void comalramrom_detach(void);
void comalramrom_powerup(void);

/* Memory window (ROML/ROMH) */
uint8_t comalramrom_roml_read(uint16_t addr);
void    comalramrom_roml_store(uint16_t addr, uint8_t value);
uint8_t comalramrom_romh_read(uint16_t addr);
void    comalramrom_romh_store(uint16_t addr, uint8_t value);

/* Snapshot */
int comalramrom_snapshot_write_module(struct snapshot_s *s);
int comalramrom_snapshot_read_module (struct snapshot_s *s);

#endif /* VICE_COMALRAMROM_H */
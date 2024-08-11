
void inc_ref_count(int pa);
void dec_ref_count(int pa);
int get_ref_count(int pa);
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);
pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);
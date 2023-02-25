#include "intern/eval/deg_eval_runtime_backup_gpencil.h"
#include "intern/depsgraph.h"

#include "BKE_gpencil.h"
#include "BKE_gpencil_update_cache.h"

#include "types_gpen_types.h"

namespace dune::deg {

DPenBackup::GPenBackup(const Depsgraph *depsgraph) : depsgraph(depsgraph)
{
}

void GPencilBackup::init_from_gpencil(bGPdata * /*gpd*/)
{
}

void GPencilBackup::restore_to_gpencil(bGPdata *gpd)
{
  bGPdata *gpd_orig = reinterpret_cast<bGPdata *>(gpd->id.orig_id);

  /* We check for the active depsgraph here to avoid freeing the cache on the original object
   * multiple times. This free is only needed for the case where we tagged a full update in the
   * update cache and did not do an update-on-write. */
  if (depsgraph->is_active) {
    BKE_gpencil_free_update_cache(gpd_orig);
  }
  /* Doing a copy-on-write copies the update cache pointer. Make sure to reset it
   * to NULL as we should never use the update cache from eval data. */
  gpd->runtime.update_cache = nullptr;
  /* Make sure to update the original runtime pointers in the eval data. */
  BKE_gpencil_data_update_orig_pointers(gpd_orig, gpd);
}

}  // namespace blender::deg

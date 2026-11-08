/* Default-off same-device image leases. Included after the actual live owner
 * guard so no SDK/device is constructed by this seam. */
static DXGI_FORMAT ld_color_format(u32);
typedef struct ld_native_image_record {
  uint64_t id, generation;
  uint32_t pins;
  rsx_native_d3d12_image payload;
  struct ld_native_image_record *next;
} ld_native_image_record;
/* Current color/depth sets plus two complete retained historical sets. At
 * exhaustion reject a new snapshot, preserving every existing pinned lease. */
enum { LD_NATIVE_IMAGE_RECORDS = 6 * MAX_SURFACES };
static ld_native_image_record *s_native_images;
static uint64_t s_native_image_sequence;
static ID3D12Resource *ld_native_allocation(uint64_t id, uint64_t *generation,
                                            uint32_t *state, uint32_t *known,
                                            uint32_t *planes) {
  if (id >= 1 && id <= g.n_surfaces) {
    surface_t *s = &g.surfaces[id - 1];
    *generation = s->native_generation;
    *state = s->native_state;
    *known = s->native_state_known;
    *planes = s->native_clear_planes;
    return s->tex;
  }
  if (id > MAX_SURFACES && id <= MAX_SURFACES + g.n_zdepths) {
    zdepth_t *s = &g.zdepths[id - MAX_SURFACES - 1];
    *generation = s->native_generation;
    *state = s->native_state;
    *known = s->native_state_known;
    *planes = s->native_clear_planes;
    return s->tex;
  }
  return NULL;
}
static int ld_native_image_current(ld_native_image_record *r) {
  uint64_t generation = 0;
  uint32_t state = 0, known = 0, planes = 0;
  ID3D12Resource *resource =
      ld_native_allocation(r->id, &generation, &state, &known, &planes);
  return resource && generation == r->generation &&
         resource == r->payload.resource;
}
static void ld_native_image_gc(void) {
  ld_native_image_record **at = &s_native_images;
  while (*at) {
    ld_native_image_record *r = *at;
    if (!r->pins && !ld_native_image_current(r)) {
      *at = r->next;
      free(r);
    } else
      at = &r->next;
  }
}
static void ld_native_image_records_clear(void) {
  if (!ld_native_owner_enabled())
    return;
  /* Existing shutdown guard forbids teardown while any native pin exists. */
  if (s_native_owner.retained) {
    ld_owner_failed_boundary();
    return;
  }
  while (s_native_images) {
    ld_native_image_record *r = s_native_images;
    s_native_images = r->next;
    free(r);
  }
}
static uint64_t ld_native_new_generation(void) {
  if (!ld_native_owner_enabled())
    return 0;
  if (s_native_image_sequence == UINT64_MAX) {
    ld_owner_failed_boundary();
    return 0;
  }
  return ++s_native_image_sequence;
}
static void ld_native_track_state(ID3D12Resource *resource,
                                  const D3D12_RESOURCE_BARRIER *b,
                                  uint32_t *state, uint32_t *known) {
  if (resource != b->Transition.pResource)
    return;
  if (b->Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
      b->Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
      *state != (uint32_t)b->Transition.StateBefore) {
    *known = 0;
    return;
  }
  *state = (uint32_t)b->Transition.StateAfter;
}
static void
ld_native_resource_barriers(UINT count,
                            const D3D12_RESOURCE_BARRIER *barriers) {
  /* Exactly the original driver call and order. Only metadata is added. */
  g.list->lpVtbl->ResourceBarrier(g.list, count, barriers);
  if (!ld_native_owner_enabled())
    return;
  for (UINT b = 0; b < count; ++b)
    if (barriers[b].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
      for (uint32_t i = 0; i < g.n_surfaces; ++i)
        if (g.surfaces[i].tex)
          ld_native_track_state(g.surfaces[i].tex, &barriers[b],
                                &g.surfaces[i].native_state,
                                &g.surfaces[i].native_state_known);
      for (uint32_t i = 0; i < g.n_zdepths; ++i)
        if (g.zdepths[i].tex)
          ld_native_track_state(g.zdepths[i].tex, &barriers[b],
                                &g.zdepths[i].native_state,
                                &g.zdepths[i].native_state_known);
    }
}
static ld_native_image_record *ld_native_record(uint64_t id,
                                                uint64_t generation) {
  for (ld_native_image_record *r = s_native_images; r; r = r->next)
    if (r->id == id && r->generation == generation)
      return r;
  return NULL;
}
static int ld_native_identity(uint64_t life, uint64_t id, uint64_t generation) {
  if (!life || life != s_native_owner.generation || !generation ||
      s_native_owner.closing)
    return -3;
  if (s_native_owner.failed || !g.ready)
    return -6;
  ld_native_image_record *r = ld_native_record(id, generation);
  return r && ld_native_image_current(r) ? 0 : -3;
}
static int ld_image_retain(void *user, uint64_t life, uint64_t id,
                           uint64_t generation) {
  if (user != &s_native_owner)
    return -3;
  if (s_native_thread.gate || !TryAcquireSRWLockExclusive(&s_native_owner.lock))
    return -4;
  int status = ld_native_identity(life, id, generation);
  ld_native_image_record *r = ld_native_record(id, generation);
  if (!status && (s_native_owner.retained == UINT_MAX || r->pins == UINT_MAX))
    status = -4;
  if (!status) {
    ID3D12Resource *resource = r->payload.resource;
    resource->lpVtbl->AddRef(resource);
    ++r->pins;
    ++s_native_owner.retained;
  }
  ReleaseSRWLockExclusive(&s_native_owner.lock);
  return status;
}
static void ld_image_release(void *user, uint64_t life, uint64_t id,
                             uint64_t generation) {
  if (user != &s_native_owner)
    return;
  const int held = s_native_thread.gate == &s_native_owner &&
                   (s_native_thread.native || s_native_thread.original_depth);
  if (s_native_thread.gate && !held)
    return;
  if (!held)
    AcquireSRWLockExclusive(&s_native_owner.lock);
  ld_native_image_record *r = ld_native_record(id, generation);
  if (life == s_native_owner.generation && r && r->pins &&
      s_native_owner.retained) {
    ID3D12Resource *resource = r->payload.resource;
    --r->pins;
    --s_native_owner.retained;
    resource->lpVtbl->Release(resource);
    ld_native_image_gc();
  }
  if (!held)
    ReleaseSRWLockExclusive(&s_native_owner.lock);
}
static int ld_image_validate(void *user, uint64_t life, uint64_t id,
                             uint64_t generation) {
  if (user != &s_native_owner)
    return -3;
  if (s_native_thread.gate != &s_native_owner || !s_native_thread.native)
    return -4;
  int status = ld_native_identity(life, id, generation);
  if (status)
    return status;
  uint64_t current = 0;
  uint32_t state = 0, known = 0, planes = 0;
  (void)ld_native_allocation(id, &current, &state, &known, &planes);
  ld_native_image_record *r = ld_native_record(id, generation);
  return known && state == r->payload.acquire_state ? 0 : -2;
}
int rsx_live_draw_validate_image_planes(void *user, uint64_t life, uint64_t id,
                                        uint64_t generation,
                                        uint32_t required) {
  if (!required || (required & ~7u))
    return -1;
  int status = ld_image_validate(user, life, id, generation);
  if (status)
    return status;
  uint64_t current = 0;
  uint32_t state = 0, known = 0, planes = 0;
  (void)ld_native_allocation(id, &current, &state, &known, &planes);
  return (planes & required) == required ? 0 : -5;
}
static uint32_t ld_native_format(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return RSX_NATIVE_IMAGE_RGBA8;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return RSX_NATIVE_IMAGE_BGRA8;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return RSX_NATIVE_IMAGE_RGBA16F;
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
    return RSX_NATIVE_IMAGE_RGBA32F;
  case DXGI_FORMAT_R32_FLOAT:
    return RSX_NATIVE_IMAGE_R32F;
  default:
    return 0;
  }
}
int rsx_live_draw_borrow_image(uint64_t life, uint32_t handle,
                               rsx_native_image *out) {
  if (!out || out->abi_version != 1 || out->struct_size != sizeof(*out))
    return -1;
  if (!ld_native_owner_enabled())
    return -2;
  if (!ld_owner_read_begin())
    return -4;
  int status = 0;
  uint64_t generation = 0;
  uint32_t state = 0, known = 0, planes = 0;
  ID3D12Resource *resource =
      ld_native_allocation(handle, &generation, &state, &known, &planes);
  uint32_t format = 0, width = 0, height = 0, canonical = 0;
  if (handle && handle <= g.n_surfaces) {
    surface_t *s = &g.surfaces[handle - 1];
    format = ld_native_format(s->fmt);
    width = s->w;
    height = s->h;
    canonical = D3D12_RESOURCE_STATE_RENDER_TARGET;
  } else if (handle > MAX_SURFACES && handle <= MAX_SURFACES + g.n_zdepths) {
    zdepth_t *s = &g.zdepths[handle - MAX_SURFACES - 1];
    format = RSX_NATIVE_IMAGE_D32S8;
    width = s->w;
    height = s->h;
    canonical = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  }
  if (!life || life != s_native_owner.generation || s_native_owner.closing ||
      !resource || !generation)
    status = -3;
  else if (!g.ready || s_native_owner.failed)
    status = -6;
  else if (!known || !format || state != canonical)
    status = -2;
  else {
    ld_native_image_gc();
    ld_native_image_record *r = ld_native_record(handle, generation);
    if (!r) {
      unsigned records = 0;
      for (ld_native_image_record *it = s_native_images; it; it = it->next)
        ++records;
      if (records >= LD_NATIVE_IMAGE_RECORDS) {
        ld_owner_original_finish(1);
        return -5;
      }
      r = calloc(1, sizeof(*r));
      if (!r)
        status = -5;
      else {
        r->id = handle;
        r->generation = generation;
        r->payload = (rsx_native_d3d12_image){1, sizeof(r->payload), resource,
                                              canonical, canonical};
        r->next = s_native_images;
        s_native_images = r;
      }
    }
    if (!status)
      *out = (rsx_native_image){.abi_version = 1,
                                .struct_size = sizeof(*out),
                                .backend = 3,
                                .format = format,
                                .width = width,
                                .height = height,
                                .owner_lifetime = life,
                                .resource_id = handle,
                                .resource_generation = generation,
                                .image = &r->payload,
                                .userdata = &s_native_owner,
                                .retain = ld_image_retain,
                                .release = ld_image_release,
                                .validate = ld_image_validate};
  }
  ld_owner_original_finish(1);
  return status;
}
int rsx_live_draw_native_image(const rsx_native_image_key *key,
                               rsx_native_image_match *out) {
  if (!key || !out || key->abi_version != 1 ||
      key->struct_size != sizeof(*key) || out->abi_version != 1 ||
      out->struct_size != sizeof(*out) || key->kind > 1 || key->location > 1 ||
      key->format > 31 || key->reserved[0] || key->reserved[1] || !key->width ||
      !key->height ||
      (key->kind == RSX_NATIVE_COLOR_IMAGE
           ? (key->aa_grid != 0x101u && key->aa_grid != 0x102u &&
              key->aa_grid != 0x202u)
           : key->aa_grid != 0))
    return -1;
  if (!ld_native_owner_enabled())
    return -2;
  if (!ld_owner_read_begin())
    return -4;
  int status = 0;
  uint32_t handle = 0, planes = 0;
  if (!g.ready || s_native_owner.failed)
    status = -6;
  else if (s_native_owner.closing)
    status = -3;
  else if (g.rsx.in_begin_end)
    status = -4;
  else if (key->kind == RSX_NATIVE_COLOR_IMAGE) {
    for (uint32_t i = 0; i < g.n_surfaces; ++i) {
      surface_t *s = &g.surfaces[i];
      if (s->tex && s->location == key->location && s->offset == key->offset &&
          s->pitch == key->pitch && s->w == key->width && s->h == key->height &&
          s->fmt == ld_color_format(key->format) &&
          s->native_source_format == key->format &&
          s->native_aa_grid == key->aa_grid) {
        if (handle) {
          status = 0;
          handle = 0;
          break;
        }
        handle = i + 1;
        planes = s->native_clear_planes;
      }
    }
  } else if (key->aa_grid == 0) {
    for (uint32_t i = 0; i < g.n_zdepths; ++i) {
      zdepth_t *s = &g.zdepths[i];
      if (s->tex && s->location == key->location && s->offset == key->offset &&
          s->native_pitch == key->pitch && s->w == key->width &&
          s->h == key->height && s->native_source_format == key->format) {
        if (handle) {
          handle = 0;
          break;
        }
        handle = MAX_SURFACES + i + 1;
        planes = s->native_clear_planes;
      }
    }
  } else
    status = -1;
  if (!status && handle) {
    *out = (rsx_native_image_match){1,          sizeof(*out), key->kind, handle,
                                    key->width, key->height,  planes,    0};
    status = 1;
  }
  ld_owner_original_finish(1);
  return status;
}
int rsx_live_draw_native_registers(const uint32_t *methods, uint32_t *values,
                                   uint32_t count) {
  if (!methods || !values || !count || count > 256)
    return -1;
  for (uint32_t i = 0; i < count; ++i)
    if ((methods[i] & 3u) || methods[i] > 0x1ffcu)
      return -1;
  if (!ld_native_owner_enabled())
    return -2;
  if (!ld_owner_read_begin())
    return -4;
  int status = !g.ready || s_native_owner.failed ? -6
               : s_native_owner.closing          ? -3
               : g.rsx.in_begin_end              ? -4
                                                 : 0;
  if (!status) {
    uint32_t snapshot[256];
    for (uint32_t i = 0; i < count; ++i)
      snapshot[i] = rsx_dsp_reg(&g.rsx, methods[i]);
    memcpy(values, snapshot, count * sizeof(*values));
  }
  ld_owner_original_finish(1);
  return status;
}
int rsx_live_draw_native_canvas(uint32_t dimensions[2]) {
  if (!dimensions)
    return -1;
  if (!ld_native_owner_enabled())
    return -2;
  if (!ld_owner_read_begin())
    return -4;
  int status = !g.ready || s_native_owner.failed ? -6
               : s_native_owner.closing          ? -3
               : g.rsx.in_begin_end              ? -4
                                                 : 0;
  if (!status) {
    dimensions[0] = g.width;
    dimensions[1] = g.height;
  }
  ld_owner_original_finish(1);
  return status;
}
int rsx_live_draw_native_boundary(uint64_t *ticket) {
  if (!ticket)
    return -1;
  *ticket = 0;
  if (!ld_native_owner_enabled())
    return -2;
  if (!ld_owner_read_begin())
    return -4;
  int status = !g.ready || s_native_owner.failed || !rsx_picture_ready() ? -6
               : s_native_owner.closing                                  ? -3
                                                                         : 0;
  if (!status && (g.rsx.in_begin_end || s_native_owner.dirty || !g.fence ||
                  !g.fence_value))
    status = -4;
  if (!status) {
    uint64_t completed = g.fence->lpVtbl->GetCompletedValue(g.fence);
    if (completed == UINT64_MAX ||
        FAILED(g.dev->lpVtbl->GetDeviceRemovedReason(g.dev))) {
      ld_owner_failed_boundary();
      status = -6;
    } else if (completed < g.fence_value)
      status = -4;
    else
      *ticket = g.fence_value;
  }
  if (status == -6)
    ld_owner_failed_boundary();
  ld_owner_original_finish(1);
  return status;
}

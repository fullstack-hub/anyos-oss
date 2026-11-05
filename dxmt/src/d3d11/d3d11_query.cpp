#include "d3d11_query.hpp"
#include "d3d11_device.hpp"
#include "dxmt_occlusion_query.hpp"

namespace dxmt {

class OcclusionQuery : public MTLD3DQueryBase<MTLD3D11OcclusionQuery> {
  using MTLD3DQueryBase<MTLD3D11OcclusionQuery>::MTLD3DQueryBase;

  QueryState state_ = QueryState::Signaled;

  Rc<VisibilityResultQuery> query_ = new VisibilityResultQuery();
  uint64_t accumulated_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE ? sizeof(BOOL) : sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Building) {
      return DXGI_ERROR_INVALID_CALL;
    }
    if (state_ == QueryState::Signaled || query_->getValue(&accumulated_value_)) {
      if (desc_.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
        *((BOOL *)data) = accumulated_value_ != 0;
      } else {
        *((uint64_t *)data) = accumulated_value_;
      }
      query_ = new VisibilityResultQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual VisibilityResultQuery *
  Begin() override {
    if (state_ == QueryState::Signaled) {
      state_ = QueryState::Building;
      return query_.ptr();
    }
    if (state_ == QueryState::Issued) {
      // discard previous issued query
      state_ = QueryState::Building;
      query_ = new VisibilityResultQuery();
      return query_.ptr();
    }
    // FIXME: it's effectively ignoring  Begin() after Begin()
    return nullptr;
  };

  virtual VisibilityResultQuery *
  End() override {
    if (state_ == QueryState::Signaled) {
      // ignore  a single End()
      accumulated_value_ = 0;
      return nullptr;
    }
    if (state_ == QueryState::Issued) {
      // FIXME: it's effectively ignoring End() after End()
      return nullptr;
    }
    state_ = QueryState::Issued;
    return query_.ptr();
  };

  virtual void DoDeferredQuery(VisibilityResultQuery *deferred_query) override{
    accumulated_value_ = 0;
    state_ = QueryState::Issued;
    query_ = deferred_query;
  };
};

HRESULT
CreateOcclusionQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new OcclusionQuery(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

class MTLD3D11TimestampQueryImpl : public MTLD3DQueryBase<MTLD3D11TimestampQuery> {
  using MTLD3DQueryBase<MTLD3D11TimestampQuery>::MTLD3DQueryBase;

  QueryState state_ = QueryState::Signaled;

  Rc<TimestampQuery> query_ = new TimestampQuery();
  uint64_t latest_value_ = 0;

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return sizeof(UINT64);
  };

  virtual HRESULT
  GetData(void *data) override {
    if (state_ == QueryState::Signaled || query_->getValue(&latest_value_)) {
      *((uint64_t *)data) = latest_value_;
      query_ = new TimestampQuery();
      state_ = QueryState::Signaled;
      return S_OK;
    }
    return S_FALSE;
  }

  virtual TimestampQuery *
  End() override {
    if (state_ == QueryState::Issued) {
      // discard previous query
      query_ = new TimestampQuery();
    }
    state_ = QueryState::Issued;
    return query_.ptr();
  };
};

class MTLD3D11StreamOutputQueryImpl : public MTLD3DQueryBase<MTLD3D11StreamOutputQuery> {
public:
  MTLD3D11StreamOutputQueryImpl(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc) :
      MTLD3DQueryBase<MTLD3D11StreamOutputQuery>(pDevice, pDesc),
      device_(pDevice->GetMTLDevice()) {}

  virtual UINT STDMETHODCALLTYPE
  GetDataSize() override {
    return predicate() ? sizeof(BOOL) : sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
  };

  virtual StreamOutputQuery *
  Begin() override {
    query_ = new StreamOutputQuery(device_);
    state_ = QueryState::Building;
    return stream0() ? query_.ptr() : nullptr;
  }

  virtual StreamOutputQuery *
  End(uint64_t event_seq_id) override {
    if (state_ != QueryState::Building) {
      // End alone brackets nothing
      query_ = new StreamOutputQuery(device_);
    }
    state_ = QueryState::Issued;
    signaled_at_ = event_seq_id;
    return query_.ptr();
  }

  virtual HRESULT
  GetData(void *data, uint64_t signaled_event_seq_id) override {
    if (state_ == QueryState::Building)
      return DXGI_ERROR_INVALID_CALL;
    if (state_ == QueryState::Issued) {
      if (signaled_event_seq_id < signaled_at_)
        return S_FALSE;
      written_ = query_->values()[0];
      needed_ = query_->values()[1];
      state_ = QueryState::Signaled;
    }
    if (data) {
      if (predicate()) {
        *(BOOL *)data = needed_ > written_;
      } else {
        auto stats = (D3D11_QUERY_DATA_SO_STATISTICS *)data;
        stats->NumPrimitivesWritten = written_;
        stats->PrimitivesStorageNeeded = needed_;
      }
    }
    return S_OK;
  }

private:
  bool
  predicate() const {
    switch (desc_.Query) {
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
      return true;
    default:
      return false;
    }
  }

  bool
  stream0() const {
    switch (desc_.Query) {
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
      return true;
    default:
      return false;
    }
  }

  WMT::Device device_;
  QueryState state_ = QueryState::Signaled;
  Rc<StreamOutputQuery> query_;
  uint64_t signaled_at_ = 0;
  uint64_t written_ = 0;
  uint64_t needed_ = 0;
};

HRESULT
CreateStreamOutputQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new MTLD3D11StreamOutputQueryImpl(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

HRESULT
CreateTimestampQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC1 *pDesc, ID3D11Query1 **ppQuery) {
  if (ppQuery) {
    *ppQuery = ref(new MTLD3D11TimestampQueryImpl(pDevice, pDesc));
    return S_OK;
  }
  return S_FALSE;
}

} // namespace dxmt
#include "locality_finder.hpp"

#include "../indexer/ftypes_matcher.hpp"
#include "../indexer/features_vector.hpp"

#include "../geometry/distance_on_sphere.hpp"


namespace search
{

double const MAX_RADIUS_CITY = 30000.0;

class DoLoader
{
public:
  DoLoader(LocalityFinder const & finder, FeaturesVector const & loader, LocalityFinder::Cache & cache, m2::RectD const & rect)
    : m_finder(finder), m_loader(loader), m_cache(cache), m_rect(rect)
  {
  }

  void operator() (uint32_t id)
  {
    FeatureType ft;
    m_loader.Get(id, ft);

    if (ft.GetFeatureType() != feature::GEOM_POINT)
      return;

    using namespace ftypes;
    switch (IsLocalityChecker::Instance().GetType(ft))
    {
    case CITY:
    case TOWN:
      break;
    default:  // cache only cities and towns at this moment
      return;
    }

    if (m_cache.m_loaded.count(id) > 0)
      return; // already loaded

    uint32_t const population = ftypes::GetPopulation(ft);
    if (population == 0)
      return;

    double const radius = ftypes::GetRadiusByPopulation(population);
    m2::RectD const rect = MercatorBounds::RectByCenterXYAndSizeInMeters(ft.GetCenter(), radius);
    if (!rect.IsIntersect(m_rect))
      return;

    // read item
    string name;
    if (!ft.GetName(m_finder.m_lang, name))
      if (!ft.GetName(0, name))
        return;

    LocalityItem item(rect, population, id, name);
    m_cache.m_tree.Add(item, item.GetLimitRect());
    m_cache.m_loaded.insert(id);
  }

private:
  LocalityFinder const & m_finder;
  FeaturesVector const & m_loader;
  LocalityFinder::Cache & m_cache;
  m2::RectD m_rect;
};


class DoSelectLocality
{
public:
  DoSelectLocality(string & name, m2::PointD const & p)
    : m_name(name) , m_point(p), m_bestValue(numeric_limits<double>::max())
  {
  }

  void operator() (LocalityItem const & item)
  {
    m2::PointD const c = item.m_rect.Center();
    double const d = ms::DistanceOnEarth(MercatorBounds::YToLat(c.y),
                                         MercatorBounds::XToLon(c.x),
                                         MercatorBounds::YToLat(m_point.y),
                                         MercatorBounds::XToLon(m_point.x));

    double const value = ftypes::GetPopulationByRadius(d) / static_cast<double>(item.m_population);
    if (value < m_bestValue)
    {
      m_bestValue = value;
      m_name = item.m_name;
    }
  }

private:
  string & m_name;
  m2::PointD m_point;
  double m_bestValue;
};



LocalityItem::LocalityItem(m2::RectD const & rect, uint32_t population, ID id, string const & name)
  : m_rect(rect), m_name(name), m_population(population), m_id(id)
{

}

LocalityFinder::LocalityFinder(Index const * pIndex)
  : m_pIndex(pIndex), m_lang(0)
{
}

void LocalityFinder::CorrectMinimalRect(m2::RectD & rect) const
{
  m2::RectD const rlt = MercatorBounds::RectByCenterXYAndSizeInMeters(rect.LeftTop(), MAX_RADIUS_CITY);
  m2::RectD const rrb = MercatorBounds::RectByCenterXYAndSizeInMeters(rect.RightBottom(), MAX_RADIUS_CITY);
  rect = m2::RectD(MercatorBounds::ClampX(rlt.minX()),
                   MercatorBounds::ClampY(rrb.minY()),
                   MercatorBounds::ClampX(rrb.maxX()),
                   MercatorBounds::ClampY(rlt.maxY()));
}

void LocalityFinder::RecreateCache(Cache & cache, m2::RectD rect) const
{
  vector<MwmInfo> mwmInfo;
  m_pIndex->GetMwmInfo(mwmInfo);

  cache.Clear();

  CorrectMinimalRect(rect);
  covering::CoveringGetter cov(rect, covering::ViewportWithLowLevels);

  for (MwmSet::MwmId mwmId = 0; mwmId < mwmInfo.size(); ++mwmId)
  {
    typedef feature::DataHeader HeaderT;
    Index::MwmLock mwmLock(*m_pIndex, mwmId);
    MwmValue * pMwm = mwmLock.GetValue();
    if (pMwm && pMwm->GetHeader().GetType() == HeaderT::world)
    {
      HeaderT const & header = pMwm->GetHeader();

      int const scale = header.GetLastScale();   // scales::GetUpperWorldScale()
      covering::IntervalsT const & interval = cov.Get(scale);

      ScaleIndex<ModelReaderPtr> index(pMwm->m_cont.GetReader(INDEX_FILE_TAG), pMwm->m_factory);

      FeaturesVector loader(pMwm->m_cont, header);

      cache.m_rect = rect;
      for (size_t i = 0; i < interval.size(); ++i)
      {
        index.ForEachInIntervalAndScale(DoLoader(*this, loader, cache, rect),
                                        interval[i].first, interval[i].second,
                                        scale);
      }
    }
  }
}

void LocalityFinder::SetViewportByIndex(m2::RectD const & rect, size_t idx)
{
  ASSERT_LESS(idx, (size_t)MAX_VIEWPORT_COUNT, ());
  RecreateCache(m_cache[idx], rect);
}

void LocalityFinder::GetLocalityInViewport(const m2::PointD & pt, string & name) const
{
  for (size_t i = 0; i < MAX_VIEWPORT_COUNT; ++i)
    m_cache[i].GetLocality(pt, name);
}

void LocalityFinder::GetLocalityCreateCache(const m2::PointD & pt, string & name) const
{
  // search in temporary caches and find most unused cache
  size_t minUsageIdx = 0;
  size_t minUsage = numeric_limits<size_t>::max();
  for (size_t idx = 0; idx < MAX_CACHE_TMP_COUNT; ++idx)
  {
    Cache const & cache = m_cache_tmp[idx];
    cache.GetLocality(pt, name);
    if (!name.empty())
      return;

    if (cache.m_usage < minUsage)
    {
      minUsage = cache.m_usage;
      minUsageIdx = idx;
    }
  }

  Cache & cache = m_cache_tmp[minUsageIdx];
  RecreateCache(cache, MercatorBounds::RectByCenterXYAndSizeInMeters(pt, MAX_RADIUS_CITY));
  cache.GetLocality(pt, name);
}

void LocalityFinder::ClearCacheAll()
{
  for (size_t i = 0; i < MAX_VIEWPORT_COUNT; ++i)
    ClearCache(i);

  for (size_t i = 0; i < MAX_CACHE_TMP_COUNT; ++i)
    m_cache_tmp[i].Clear();
}

void LocalityFinder::ClearCache(size_t idx)
{
  ASSERT_LESS(idx, (size_t)MAX_VIEWPORT_COUNT, ());
  m_cache[idx].Clear();
}



void LocalityFinder::Cache::Clear()
{
  m_usage = 0;
  m_tree.Clear();
  m_loaded.clear();
}

void LocalityFinder::Cache::GetLocality(m2::PointD const & pt, string & name) const
{
  if (!m_rect.IsPointInside(pt))
    return;

  ++m_usage;
  m_tree.ForEachInRect(m2::RectD(pt, pt), DoSelectLocality(name, pt));
}

}

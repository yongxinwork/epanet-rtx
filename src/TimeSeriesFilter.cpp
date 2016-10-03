//
//  TimeSeriesFilter.cpp
//  epanet-rtx
//

#include "TimeSeriesFilter.h"
#include <boost/foreach.hpp>

using namespace RTX;
using namespace std;

const int _tsfilter_maxStrides = 7; // FIXME 💩

TimeSeriesFilter::TimeSeriesFilter() {
  _resampleMode = TimeSeriesResampleModeLinear;
}

Clock::_sp TimeSeriesFilter::clock() {
  return _clock;
}
void TimeSeriesFilter::setClock(Clock::_sp clock) {
  if (clock) {
    _clock = clock;
  }
  else {
    _clock.reset();
  }
  this->invalidate();
}

TimeSeries::TimeSeriesResampleMode TimeSeriesFilter::resampleMode() {
  return _resampleMode;
}

void TimeSeriesFilter::setResampleMode(TimeSeries::TimeSeriesResampleMode mode) {
  
  if (_resampleMode != mode) {
    this->invalidate();
  }
  
  _resampleMode = mode;
}


// filters can invalidate themselves.
void TimeSeriesFilter::setUnits(RTX::Units newUnits) {
  if (! (newUnits == this->units())) {
    //this->invalidate();
    TimeSeries::setUnits(newUnits);
  }
}

TimeSeries::_sp TimeSeriesFilter::source() {
  return _source;
}
void TimeSeriesFilter::setSource(TimeSeries::_sp ts) {
  if (ts && !this->canSetSource(ts)) {
    return;
  }
  _source = ts;
  if (ts) {
    this->invalidate();
    this->didSetSource(ts);
  }
}


vector<Point> TimeSeriesFilter::points(TimeRange range) {
  vector<Point> filtered;
  vector<Point> cached;
  
  if (! this->source()) {
    return filtered;
  }
  
  set<time_t> pointTimes;
  
  if (this->canDropPoints()) {
    // optmized fetching: we know we're going to use these same points...
    PointCollection c = this->filterPointsInRange(range);
    cached = c.points;
    pointTimes = c.times();
  }
  else {
    pointTimes = this->timeValuesInRange(range);
    cached = TimeSeries::points(range); // base class call -> find any pre-cached points
  }
  
  // important optimization. if this range has already been constructed and cached, then don't recreate it.
  bool alreadyCached = false;
  if (cached.size() == pointTimes.size()) {
    // looks good, let's make sure that all time values line up.
    alreadyCached = true;
    BOOST_FOREACH(const Point& p, cached) {
      if (pointTimes.count(p.time) == 0) {
        alreadyCached = false;
        break; // break foreach, with false "alreadyCached" flag
      }
    }
  }
  if (alreadyCached) {
    return cached; // we're done here. all points are present.
  }
  
  PointCollection outCollection = this->filterPointsInRange(range);
  this->insertPoints(outCollection.points);
  outCollection = outCollection.trimmedToRange(range); // safeguard if filter doesn't respected the range

  return outCollection.points;
}


Point TimeSeriesFilter::pointBefore(time_t time) {
  Point p;
  p.time = time;
  time_t seekTime = time;
  
  if (!this->source()) {
    return Point();
  }
  
  if (this->canDropPoints()) {
    // search iteratively
    
    time_t stride = 60*60*24; // 1-day
    PointCollection c;
    
    TimeRange q(time - stride, time - 1);
    
    while ( q.start > time - (stride * _tsfilter_maxStrides) ) {
      if (this->source()->timeBefore(q.end + 1) == 0) {
        break; // actually no points. futile to search.
      }
      c = TimeSeries::pointCollection(q);
      if (c.count() > 0) {
        break; // found something
      }
      q.start -= stride;
      q.end -= stride;
    }
    
    // if we found something:
    if (c.count() > 0) {
      p = c.points.back();
    }
  }
  else {
    // points in, points out
    seekTime = this->timeBefore(seekTime);
    p = this->point(seekTime);
  }
  
  return p;
}

Point TimeSeriesFilter::pointAfter(time_t time) {
  Point p;
  p.time = time;
  time_t seekTime = time;
  
  if (!this->source()) {
    return Point();
  }
  
  
  if (this->canDropPoints()) {
    // search iteratively - this is basic functionality. Override ::pointAfter for special cases or optimized uses.
    
    time_t stride = 60*60*24; // 1-day
    
    PointCollection c;
    TimeRange q(time + 1, time + stride);
    
    while ( q.end < time + (stride * _tsfilter_maxStrides) ) {
      if (this->source()->timeAfter(q.start - 1) == 0) {
        break; // actually no points. futile to search.
      }
      c = this->pointCollection(q);
      if (c.count() > 0) {
        break; // found something
      }
      q.start += stride;
      q.end += stride;
    }
    
    // if we found something:
    if (c.count() > 0) {
      p = c.points.front();
    }
    else {
      cerr << "TimeSeriesFilter::pointAfter iterative search exceeded max strides." << endl;
    }
  }
  else {
    // points in, points out
    seekTime = this->timeAfter(seekTime);
    p = this->point(seekTime);
  }
  
  return p;
}


#pragma mark - Pseudo-Delegate methods

bool TimeSeriesFilter::willResample() {
  
  // if i don't have a clock, then the answer is obvious
  if (!this->clock()) {
    return false;
  }
  
  // no source, also not resampling. or rather, it's a moot point.
  if (!this->source()) {
    return false;
  }
  
  TimeSeriesFilter::_sp sourceFilter = std::dynamic_pointer_cast<TimeSeriesFilter>(this->source());
  
  if (sourceFilter) {
    // my source is a filter, so it may have a clock
    Clock::_sp sourceClock = sourceFilter->clock();
    if (!sourceClock && this->clock()) {
      // no source clock, but i have a clock. i resample
      return true;
    }
    if (sourceClock && sourceClock != this->clock()) {
      // clocks are different, so i will resample.
      return true;
    }
  }
  else {
    // source is just a time series. i will only resample if i have a clock
    if (this->clock()) {
      return true;
    }
  }
  
  return false;
}


TimeSeries::PointCollection TimeSeriesFilter::filterPointsInRange(TimeRange range) {
  
  TimeRange queryRange = range;
  if (this->willResample()) {
    // expand range
    queryRange.start = this->source()->timeBefore(range.start + 1);
    queryRange.end = this->source()->timeAfter(range.end - 1);
  }
  
  
  // in case the source has data, but that data is truncated within the range requested
  // i.e.,   ******[******-------]-------
  if (!queryRange.isValid()) {
    if (queryRange.start == 0) {
      queryRange.start = this->source()->timeAfter(range.start); // go to next available point.
    }
    if (queryRange.end == 0) {
      queryRange.end = this->source()->timeBefore(range.end); // go to previous availble point.
    }
  }
  
  queryRange.correctWithRange(range);
  
  PointCollection data = source()->pointCollection(queryRange);
  
  bool dataOk = false;
  dataOk = data.convertToUnits(this->units());
  if (dataOk && this->willResample()) {
    set<time_t> timeValues = this->timeValuesInRange(range);
    dataOk = data.resample(timeValues, _resampleMode);
  }
  
  if (dataOk) {
    return data;
  }
  
  return PointCollection(vector<Point>(),this->units());
}


set<time_t> TimeSeriesFilter::timeValuesInRange(TimeRange range) {
  set<time_t> times;
  
  if (!range.isValid() || !this->source()) {
    return times;
  }
  
  if (this->clock()) {
    times = this->clock()->timeValuesInRange(range);
  }
  else {
    if (this->canDropPoints()) {
      PointCollection c = this->filterPointsInRange(range);
      times = c.times();
    }
    else {
      times = this->source()->timeValuesInRange(range);
    }
  }
  
  return times;
}


time_t TimeSeriesFilter::timeAfter(time_t t) {
  if (!this->source()) {
    return 0;
  }
  if (this->clock()) {
    return this->clock()->timeAfter(t);
  }
  else if (this->canDropPoints()) {
    Point pAfter = this->pointAfter(t);
    if (pAfter.isValid) {
      return pAfter.time;
    }
    else {
      return 0;
    }

  }
  else {
    return this->source()->timeAfter(t);
  }
}

time_t TimeSeriesFilter::timeBefore(time_t t) {
  if (!this->source()) {
    return 0;
  }
  if (this->clock()) {
    return this->clock()->timeBefore(t);
  }
  else if (this->canDropPoints()) {
    Point pBefore = this->pointBefore(t);
    if (pBefore.isValid) {
      return pBefore.time;
    }
    else {
      return 0;
    }
  }
  else {
    return this->source()->timeBefore(t);
  }
}


bool TimeSeriesFilter::canSetSource(TimeSeries::_sp ts) {
  bool goodSource = (ts) ? true : false;
  bool unitsOk = ( ts->units().isSameDimensionAs(this->units()) || this->units().isDimensionless() );
  return goodSource && unitsOk;
}

void TimeSeriesFilter::didSetSource(TimeSeries::_sp ts) {
  // expected behavior if this has dimensionless units, take on the units of the source.
  // we are guaranteed that the units are compatible (if canChangeToUnits was properly implmented),
  // so no further checking is required.
  if (this->units().isDimensionless()) {
    this->setUnits(ts->units());
  }
  
  // if the source is a filter, and has a clock, and I don't have a clock, then use the source's clock.
  TimeSeriesFilter::_sp sourceFilter = std::dynamic_pointer_cast<TimeSeriesFilter>(ts);
  if (sourceFilter && sourceFilter->clock() && !this->clock() && !this->canDropPoints()) {
    this->setClock(sourceFilter->clock());
  }
  
}

bool TimeSeriesFilter::canChangeToUnits(Units units) {
  
  // basic pass-thru filter just alters units, not dimension.
  
  bool canChange = true;
  
  if (this->source()) {
    canChange = this->source()->units().isSameDimensionAs(units);
  }
  
  return canChange;
}

TimeSeries::_sp TimeSeriesFilter::rootTimeSeries() {
  TimeSeries::_sp source = this->source();
  if (source) {
    source = source->rootTimeSeries();
  }
  return source;
}








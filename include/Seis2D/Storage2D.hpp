#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <variant>
#include <vector>

namespace Seis2D::Curvilinear {
enum class Precision { Float64=0, Float32=1 };
inline const char* precisionName(Precision p){return p==Precision::Float32?"FP32":"FP64";}
inline std::size_t precisionBytes(Precision p){return p==Precision::Float32?sizeof(float):sizeof(double);}

// One owned representation only. Building may use double; convert() releases it.
// Read access promotes individual values for UI/export; it never caches a double copy.
template<int Width=1> class StoredArray {
    std::variant<std::vector<double>,std::vector<float>> storage_;
public:
    using value_type=std::conditional_t<Width==1,double,std::array<double,Width>>;
    bool operator==(const StoredArray&)const=default;
    Precision precision()const{return storage_.index()?Precision::Float32:Precision::Float64;}
    template<class T> std::vector<T>& values(){return std::get<std::vector<T>>(storage_);}
    template<class T> const std::vector<T>& values()const{return std::get<std::vector<T>>(storage_);}
    template<class T> T* dataAs(){return values<T>().data();}
    template<class T> const T* dataAs()const{return values<T>().data();}
    auto data()const{
        // Legacy FP64 fixture access. A wrong precision must fail, never return
        // a silently null pointer. Production kernels use dataAs<Real>().
        if constexpr(Width==1)return values<double>().data();
        else return std::visit([](const auto& v)->const void*{return v.data();},storage_);
    }
    std::size_t size()const{return std::visit([](const auto& v){return v.size()/Width;},storage_);}
    bool empty()const{return size()==0;}
    std::size_t bytes()const{return std::visit([](const auto& v){return v.capacity()*sizeof(typename std::decay_t<decltype(v)>::value_type);},storage_);}
    void resize(std::size_t n){std::visit([&](auto& v){v.resize(n*Width);},storage_);}
    void assign(std::size_t n,double x){std::visit([&](auto& v){using T=typename std::decay_t<decltype(v)>::value_type;v.assign(n*Width,static_cast<T>(x));},storage_);}
    void zero(){std::visit([](auto& v){using T=typename std::decay_t<decltype(v)>::value_type;std::fill(v.begin(),v.end(),T(0));},storage_);}
    value_type operator[](std::size_t i)const{
        return std::visit([&](const auto& v)->value_type{
            if constexpr(Width==1)return v[i];
            else {value_type a{};for(int c=0;c<Width;++c)a[c]=v[Width*i+c];return a;}
        },storage_);
    }
    void set(std::size_t i,const value_type& x){std::visit([&](auto& v){
        using T=typename std::decay_t<decltype(v)>::value_type;
        if constexpr(Width==1)v[i]=static_cast<T>(x);
        else for(int c=0;c<Width;++c)v[Width*i+c]=static_cast<T>(x[c]);
    },storage_);}
    void convert(Precision p){
        if(precision()==p)return;
        if(p==Precision::Float32){
            const auto& source=values<double>();std::vector<float> next;next.reserve(source.size());
            for(double v:source){const auto x=static_cast<float>(v);if(!std::isfinite(x))throw std::runtime_error("2D FP32 model/state coefficient overflow; select FP64 or revise model.");next.push_back(x);}
            storage_=std::move(next);
        }else {const auto& source=values<float>();std::vector<double> next(source.begin(),source.end());storage_=std::move(next);}
    }
    void take(std::vector<double> v,Precision p){storage_=std::move(v);convert(p);}
    std::vector<double> toDouble()const{return std::visit([](const auto& v){return std::vector<double>(v.begin(),v.end());},storage_);}
    struct const_iterator {
        using iterator_category=std::random_access_iterator_tag;
        using value_type=typename StoredArray::value_type;using difference_type=std::ptrdiff_t;
        using reference=value_type;using pointer=void;
        const StoredArray* owner{};difference_type at{};
        value_type operator*()const{return (*owner)[at];}
        value_type operator[](difference_type n)const{return (*owner)[at+n];}
        const_iterator& operator++(){++at;return *this;}const_iterator operator++(int){auto x=*this;++*this;return x;}
        const_iterator& operator--(){--at;return *this;}const_iterator operator--(int){auto x=*this;--*this;return x;}
        const_iterator& operator+=(difference_type n){at+=n;return *this;}const_iterator& operator-=(difference_type n){at-=n;return *this;}
        friend const_iterator operator+(const_iterator a,difference_type n){return a+=n;}
        friend const_iterator operator+(difference_type n,const_iterator a){return a+=n;}
        friend const_iterator operator-(const_iterator a,difference_type n){return a-=n;}
        friend difference_type operator-(const_iterator a,const_iterator b){return a.at-b.at;}
        friend bool operator==(const_iterator a,const_iterator b){return a.owner==b.owner&&a.at==b.at;}
        friend auto operator<=>(const_iterator a,const_iterator b){return a.at<=>b.at;}
    };
    const_iterator begin()const{return {this,0};}const_iterator end()const{return {this,static_cast<std::ptrdiff_t>(size())};}
};
using ScalarArray=StoredArray<1>;
using VectorArray=StoredArray<2>;
}

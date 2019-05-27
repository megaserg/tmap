#include "lshforest.hh"

LSHForest::LSHForest(unsigned int d, unsigned int l, bool store, bool file_backed) 
: d_(d), l_(l), size_(0), store_(store), file_backed_(file_backed), hashtables_(l), hashranges_(l), sorted_hashtable_pointers_(l)
{
    if (l_ > d_)
        throw std::invalid_argument("l must be equal to or greater than d");

    k_ = (unsigned int)(d_ / l_);

    for (unsigned int i = 0; i < l_; i++)
    {
        hashtables_[i] = spp::sparse_hash_map<std::vector<uint8_t>, std::vector<uint32_t>, MyHash>();
        hashranges_[i] = std::make_tuple(i * k_, (i + 1) * k_);
    }
}

void LSHForest::Add(std::vector<uint32_t> &vec)
{
    if (store_)
    {
        if (file_backed_)
        {
            std::ofstream fout("data.dat", std::ios::ate | std::ios::app | std::ios::binary);
            fout.write((char*)&vec[0], vec.size() * sizeof(uint32_t));
            fout.close();
        }
        else
            data_.emplace_back(vec);
    }

    for (size_t i = 0; i < hashtables_.size(); i++)
    {
        std::vector<uint32_t> range(vec.begin() + std::get<0>(hashranges_[i]),
                                    vec.begin() + std::get<1>(hashranges_[i]));
        
        hashtables_[i][Hash(Swap(range))].emplace_back(size_);
    }

    size_++;
    clean_ = false;
}

void LSHForest::BatchAdd(std::vector<std::vector<uint32_t>> &vecs)
{
    size_t length = vecs.size();
    size_t data_length = size_;

    std::vector<uint32_t> keys(length);
    for (size_t i = 0; i < length; i++)
        keys[i] = data_length + i;

    if (store_) 
    {
        if (file_backed_)
        {
            std::ofstream fout("data.dat", std::ios::ate | std::ios::app | std::ios::binary);
            for (size_t i = 0; i < length; i++)
                fout.write((char*)&vecs[i][0], vecs[i].size() * sizeof(uint32_t));
            fout.close();
        }
        else 
        {
            for (size_t i = 0; i < length; i++)
                data_.emplace_back(vecs[i]);
        }
    }


    size_t i, j;
    #pragma omp parallel for private(j)
    for (i = 0; i < hashtables_.size(); i++)
    {
        for (j = 0; j < keys.size(); j++)
        {
            std::vector<uint32_t> range(vecs[j].begin() + std::get<0>(hashranges_[i]),
                                        vecs[j].begin() + std::get<1>(hashranges_[i]));
        
            hashtables_[i][Hash(Swap(range))].emplace_back(keys[j]);
        }
    }

    size_ += length;
    clean_ = false;
}

void LSHForest::Index()
{
    #pragma omp parallel for
    for (size_t i = 0; i < hashtables_.size(); i++)
    {
        size_t j = 0;
        sorted_hashtable_pointers_[i].resize(hashtables_[i].size());
        for (auto it = hashtables_[i].begin(); it != hashtables_[i].end(); it++)
            sorted_hashtable_pointers_[i][j++] = MapKeyPointer(it);

        std::sort(sorted_hashtable_pointers_[i].begin(), sorted_hashtable_pointers_[i].end(),
            [](MapKeyPointer a, MapKeyPointer b) { return *a < *b; });
    }

    clean_ = true;
}

std::vector<uint32_t> LSHForest::GetData(uint32_t id)
{
    if (file_backed_)
    {
        std::ifstream fin("data.dat", std::ios::in | std::ios::binary);

        size_t pos = id * d_ * sizeof(uint32_t);
        fin.seekg(pos);
        std::vector<uint32_t> result(d_);
        fin.read((char*)&result[0], result.size() * sizeof(uint32_t));
        fin.close();
        return result;
    } 
    else 
    {
        return data_[id];
    }
}

bool LSHForest::IsClean()
{
    return clean_;
}

void LSHForest::Store(const std::string &path)
{
    std::ofstream file(path, std::ios::binary);
    cereal::BinaryOutputArchive output(file);
    output(hashtables_, hashranges_, data_, store_, l_, d_, k_, clean_, size_);
    file.close();
}

void LSHForest::Restore(const std::string &path)
{
    Clear();
    std::ifstream file(path, std::ios::binary);
    cereal::BinaryInputArchive input(file);
    input(hashtables_, hashranges_, data_, store_, l_, d_, k_, clean_, size_);
    file.close();

    sorted_hashtable_pointers_ = std::vector<std::vector<MapKeyPointer>>(l_);

    Index();
}

std::vector<uint32_t> LSHForest::GetHash(uint32_t id)
{
    return GetData(id);
}

std::vector<std::pair<float, uint32_t>> LSHForest::QueryLinearScan(const std::vector<uint32_t> &vec, unsigned int k, unsigned int kc, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");
    
    auto tmp = Query(vec, k * kc);
    return LinearScan(vec, tmp, k, weighted);
}

std::vector<std::pair<float, uint32_t>> LSHForest::QueryLinearScanExclude(const std::vector<uint32_t> &vec, unsigned int k, std::vector<uint32_t> &exclude, unsigned int kc, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");
    
    auto tmp = QueryExclude(vec, exclude, k * kc);
    return LinearScan(vec, tmp, k, weighted);
}

std::vector<std::pair<float, uint32_t>> LSHForest::QueryLinearScanById(uint32_t id, unsigned int k, unsigned int kc, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");
    
    return QueryLinearScan(GetData(id), k, kc, weighted);
}

std::vector<std::pair<float, uint32_t>> LSHForest::QueryLinearScanExcludeById(uint32_t id, unsigned int k, std::vector<uint32_t> &exclude, unsigned int kc, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");
    
    return QueryLinearScanExclude(GetData(id), k, exclude, kc, weighted);
}

std::vector<std::pair<float, uint32_t>> LSHForest::LinearScan(const std::vector<uint32_t> &vec, std::vector<uint32_t> &indices, unsigned int k, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    if (k == 0 || k > indices.size())
        k = indices.size();

    std::vector<std::pair<float, uint32_t>> result(indices.size());

    for (size_t i = 0; i < indices.size(); i++)
    {
        auto data = GetData(indices[i]);
        if (weighted)
            result[i] = std::pair<float, uint32_t>(GetWeightedDistance(vec, data), indices[i]);
        else
            result[i] = std::pair<float, uint32_t>(GetDistance(vec, data), indices[i]);
    }

    std::sort(result.begin(), result.end());
    result.erase(result.begin() + k, result.end());

    return result;
}

void LSHForest::FastLinearScan(const std::vector<uint32_t> &vec, std::vector<uint32_t> &indices, std::vector<float> &weights, unsigned int k, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    if (k == 0 || k > indices.size())
        k = indices.size();

    weights.resize(indices.size());

    for (size_t i = 0; i < indices.size(); i++)
    {
        if (weighted)
            weights[i] = GetWeightedDistance(vec, GetData(indices[i]));
        else
            weights[i] = GetDistance(vec, GetData(indices[i]));
    }
}

// Does not always return k items. Is this expected?
std::vector<uint32_t> LSHForest::Query(const std::vector<uint32_t> &vec, unsigned int k)
{
    std::set<uint32_t> results;

    for (int r = k_; r > 0; r--)
    {
        QueryInternal(vec, r, results, k);

        if (results.size() >= k)
            return std::vector<uint32_t>(results.begin(), results.end());
    }

    return std::vector<uint32_t>(results.begin(), results.end());
}

std::vector<uint32_t> LSHForest::QueryExclude(const std::vector<uint32_t> &vec, std::vector<uint32_t> &exclude, unsigned int k)
{
    std::set<uint32_t> results;

    for (int r = k_; r > 0; r--)
    {
        QueryInternalExclude(vec, r, results, k, exclude);

        if (results.size() >= k)
            return std::vector<uint32_t>(results.begin(), results.end());
    }

    return std::vector<uint32_t>(results.begin(), results.end());
}

std::vector<uint32_t> LSHForest::QueryById(uint32_t id, unsigned int k)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    return Query(GetData(id), k);
}

std::vector<uint32_t> LSHForest::QueryExcludeById(uint32_t id, std::vector<uint32_t> &exclude, unsigned int k)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    return QueryExclude(GetData(id), exclude, k);
}

std::vector<std::vector<uint32_t>> LSHForest::BatchQuery(const std::vector<std::vector<uint32_t>> &vecs, unsigned int k)
{
    std::vector<std::vector<uint32_t>> results(vecs.size());

    for (unsigned int i = 0; i < vecs.size(); i++)
    {
        results[i] = Query(vecs[i], k);
    }

    return results;
}

void LSHForest::GetKNNGraph(std::vector<uint32_t> &from, std::vector<uint32_t> &to, std::vector<float> &weight, unsigned int k, unsigned int kc, bool weighted)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    from.resize(size_ * k);
    to.resize(size_ * k);
    weight.resize(size_ * k);

    #pragma omp parallel for
    for(size_t i = 0; i < size_; i++)
    {
        auto result = QueryLinearScan(GetData(i), k, kc, weighted);

        for (size_t j = 0; j < result.size(); j++)
        {
            from[k * i + j] = i;
            to[k * i + j] = result[j].second;
            weight[k * i + j] = result[j].first;
        }
    }
}

void LSHForest::QueryInternal(const std::vector<uint32_t> &vec, unsigned int r, std::set<uint32_t> &results, unsigned int k)
{
    std::vector<std::vector<uint8_t>> prefixes;

    for (size_t i = 0; i < hashranges_.size(); i++)
    {
        std::vector<uint32_t> range(vec.begin() + std::get<0>(hashranges_[i]),
                                    vec.begin() + std::get<0>(hashranges_[i]) + r);

        prefixes.emplace_back(Hash(Swap(range)));
    }

    std::size_t prefix_size = prefixes[0].size();
    
    for (size_t i = 0; i < hashtables_.size(); i++)
    {
        auto &hashtable = hashtables_[i];
        auto &sorted_hashtable = sorted_hashtable_pointers_[i];
        auto &prefix = prefixes[i];
        
        unsigned int j = BinarySearch(sorted_hashtable.size(), [&](unsigned int x) {
            auto &sh = *sorted_hashtable[x];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);

            return range >= prefix;
        });

        
        if (j < sorted_hashtable.size())
        {
            auto &sh = *sorted_hashtable[j];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);
            
            if (range != prefix)
                continue;
        }


        for (unsigned int l = j; l < sorted_hashtable.size(); l++)
        {
            auto &sh = *sorted_hashtable[l];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);
            
            if (range != prefix) 
                break;

            auto &keys = hashtable[*sorted_hashtable[l]];
            
            for (size_t m = 0; m < keys.size(); m++)
            {
                results.insert(keys[m]);

                if (results.size() >= k)
                    return;
            }
        }
    }
}

void LSHForest::QueryInternalExclude(const std::vector<uint32_t> &vec, unsigned int r, std::set<uint32_t> &results, unsigned int k, std::vector<uint32_t> &exclude)
{
    std::vector<std::vector<uint8_t>> prefixes;

    for (size_t i = 0; i < hashranges_.size(); i++)
    {
        std::vector<uint32_t> range(vec.begin() + std::get<0>(hashranges_[i]),
                                    vec.begin() + std::get<0>(hashranges_[i]) + r);

        prefixes.emplace_back(Hash(Swap(range)));
    }

    std::size_t prefix_size = prefixes[0].size();
    
    for (size_t i = 0; i < hashtables_.size(); i++)
    {
        auto &hashtable = hashtables_[i];
        auto &sorted_hashtable = sorted_hashtable_pointers_[i];
        auto &prefix = prefixes[i];

        unsigned int j = BinarySearch(sorted_hashtable.size(), [&](unsigned int x) {
            auto &sh = *sorted_hashtable[x];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);

            return range >= prefix;
        });

        
        if (j < sorted_hashtable.size())
        {
            auto &sh = *sorted_hashtable[j];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);
            
            if (range != prefix) 
                continue;
        }


        for (unsigned int l = j; l < sorted_hashtable.size(); l++)
        {
            auto &sh = *sorted_hashtable[l];
            std::vector<uint8_t> range(sh.begin(), sh.begin() + prefix_size);
            
            if (range != prefix) 
                break;

            auto &keys = hashtable[*sorted_hashtable[l]];
            
            for (size_t m = 0; m < keys.size(); m++)
            {
                if (std::find(exclude.begin(), exclude.end(), keys[m]) == exclude.end())
                    results.insert(keys[m]);

                if (results.size() >= k)
                    return;
            }
        }
    }
}

std::vector<uint8_t> LSHForest::Hash(std::vector<uint32_t> vec)
{
    auto length = sizeof(vec[0]) * vec.size();

    std::vector<uint8_t> s(length);
    std::memcpy(s.data(), vec.data(), length);
    
    return s;
}

std::vector<uint8_t> LSHForest::Hash(std::vector<std::vector<uint32_t>> vecs)
{
    // Linearize input matrix
    size_t stride = vecs[0].size();
    std::vector<uint32_t> lin(vecs.size() * stride);

    for (size_t i = 0; i < vecs.size(); i++)
        for (size_t j = 0; j < stride; j++)
            lin[i * stride + j] = vecs[i][j];

    return Hash(lin);
}

uint32_t LSHForest::Swap(uint32_t i)
{
    return ((i >> 24) & 0xff) | ((i << 8) & 0xff0000) | ((i >> 8) & 0xff00) | ((i << 24) & 0xff000000);
}

std::vector<uint32_t> LSHForest::Swap(std::vector<uint32_t> vec)
{
    std::vector<uint32_t> vec_out(vec.size());

    for (size_t i = 0; i < vec.size(); i++)
        vec_out[i] = Swap(vec[i]);

    return vec_out;
}

std::vector<std::vector<uint32_t>> LSHForest::Swap(std::vector<std::vector<uint32_t>> vecs)
{
    std::vector<std::vector<uint32_t>> vecs_out(vecs.size());

    for (size_t i = 0; i < vecs.size(); i++)
    {
        vecs_out[i] = std::vector<uint32_t>(vecs[i].size());

        for (size_t j = 0; j < vecs[i].size(); j++)
            vecs_out[i][j] = Swap(vecs[i][j]);
    }

    return vecs_out;
}

unsigned int LSHForest::BinarySearch(unsigned int n, const std::function<bool(unsigned int)> &fn)
{
    unsigned int i = 0;
    unsigned int j = n;

    while (i < j)
    {
        unsigned int h = (unsigned int)(i + (j - i) / 2);
        
        if (!fn(h))
            i = h + 1;
        else
            j = h;
    }

    return i;
}

std::vector<float> LSHForest::GetAllDistances(const std::vector<uint32_t> &vec)
{
    std::vector<float> dists(size_);

    #pragma omp parallel for
    for (size_t i = 0; i < size_; i++) 
    {
        dists[i] = GetDistance(vec, GetData(i));
    }

    return dists;
}

float LSHForest::GetDistance(const std::vector<uint32_t> &vec_a, const std::vector<uint32_t> &vec_b)
{
    float intersect = 0;

    for (unsigned int i = 0; i < d_; i++)
        if (vec_a[i] == vec_b[i])
            intersect++;

    return 1.0f - intersect / d_;
}

float LSHForest::GetWeightedDistance(const std::vector<uint32_t> &vec_a, const std::vector<uint32_t> &vec_b)
{
    float intersect = 0.0f;
    for (unsigned int i = 0; i < d_ * 2; i += 2)
        if (vec_a[i] == vec_b[i] && vec_a[i + 1] == vec_b[i + 1])
            intersect++;

    return 1.0f - 2.0f * intersect / (float)d_;
}

float LSHForest::GetDistanceById(uint32_t a, uint32_t b)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    return GetDistance(GetData(a), GetData(b));
}

float LSHForest::GetWeightedDistanceById(uint32_t a, uint32_t b)
{
    if (!store_)
        throw std::runtime_error("LSHForest was not instantiated with store=true");

    return GetWeightedDistance(GetData(a), GetData(b));
}

size_t LSHForest::size()
{
    return size_;
}

void LSHForest::Clear()
{
    hashtables_ = std::vector<spp::sparse_hash_map<std::vector<uint8_t>, std::vector<uint32_t>, MyHash>>();
    hashranges_ = std::vector<std::tuple<uint32_t, uint32_t>>();
    data_ = std::vector<std::vector<uint32_t>>();
    sorted_hashtable_pointers_ = std::vector<std::vector<MapKeyPointer>>();

    hashtables_.clear();
    hashtables_.shrink_to_fit();

    hashranges_.clear();
    hashranges_.shrink_to_fit();

    data_.clear();
    data_.shrink_to_fit();

    sorted_hashtable_pointers_.clear();
    sorted_hashtable_pointers_.shrink_to_fit();

    std::vector<spp::sparse_hash_map<std::vector<uint8_t>, std::vector<uint32_t>, MyHash>>().swap(hashtables_);
    std::vector<std::tuple<uint32_t, uint32_t>>().swap(hashranges_);
    std::vector<std::vector<uint32_t>>().swap(data_);
    std::vector<std::vector<MapKeyPointer>>().swap(sorted_hashtable_pointers_);
}

std::vector<std::vector<uint8_t>> 
LSHForest::GetKeysFromHashtable(spp::sparse_hash_map<std::vector<uint8_t>, std::vector<uint32_t>, MyHash> hashtable)
{
    std::vector<std::vector<uint8_t>> keys;

    for (auto pair : hashtable)
        keys.emplace_back(pair.first);

    return keys;
}

std::tuple<std::vector<float>, std::vector<float>, std::vector<uint32_t>, std::vector<uint32_t>, GraphProperties>
LSHForest::GetLayout(LayoutConfiguration config, bool create_mst, bool mem_dump)
{
    std::string tmp_path = std::tmpnam(nullptr);
    if (mem_dump) {
        Store(tmp_path);
    }

    auto result = LayoutFromLSHForest(*this, config, create_mst, mem_dump);

    if (mem_dump)
    {
        Restore(tmp_path);
        std::remove(tmp_path.c_str());
    }

    return result;
}
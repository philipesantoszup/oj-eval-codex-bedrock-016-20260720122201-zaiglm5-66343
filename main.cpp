#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <climits>

using namespace std;

const int BLOCK_SIZE = 4096;
const int MAX_KEYS_PER_NODE = 100;

struct FileHeader {
    size_t root_offset;
    size_t next_free_offset;
    int magic_number;
};

struct IndexKey {
    char key[65];
    int value;
    
    IndexKey() {
        memset(key, 0, sizeof(key));
        value = 0;
    }
    
    IndexKey(const string& k, int v) {
        memset(key, 0, sizeof(key));
        strncpy(key, k.c_str(), 64);
        value = v;
    }
    
    bool operator<(const IndexKey& other) const {
        int cmp = strcmp(key, other.key);
        if (cmp != 0) return cmp < 0;
        return value < other.value;
    }
    
    bool operator==(const IndexKey& other) const {
        return strcmp(key, other.key) == 0 && value == other.value;
    }
};

enum NodeType {
    INTERNAL = 0,
    LEAF = 1
};

struct NodeHeader {
    NodeType type;
    int key_count;
    size_t parent_offset;
    size_t self_offset;
};

struct InternalNode {
    NodeHeader header;
    size_t children[MAX_KEYS_PER_NODE + 1];
    IndexKey keys[MAX_KEYS_PER_NODE];
    
    InternalNode() {
        header.type = INTERNAL;
        header.key_count = 0;
        header.parent_offset = 0;
        header.self_offset = 0;
        memset(children, 0, sizeof(children));
    }
};

struct LeafNode {
    NodeHeader header;
    size_t next_leaf;
    size_t prev_leaf;
    IndexKey keys[MAX_KEYS_PER_NODE];
    
    LeafNode() {
        header.type = LEAF;
        header.key_count = 0;
        header.parent_offset = 0;
        header.self_offset = 0;
        next_leaf = 0;
        prev_leaf = 0;
    }
};

class BPlusTree {
private:
    fstream index_file;
    string filename;
    FileHeader file_header;
    
    size_t allocate_block() {
        size_t offset = file_header.next_free_offset;
        file_header.next_free_offset += BLOCK_SIZE;
        save_file_header();
        return offset;
    }
    
    void save_file_header() {
        index_file.seekp(0, ios::beg);
        index_file.write(reinterpret_cast<char*>(&file_header), sizeof(FileHeader));
        index_file.flush();
    }
    
    void load_file_header() {
        index_file.seekg(0, ios::beg);
        index_file.read(reinterpret_cast<char*>(&file_header), sizeof(FileHeader));
    }
    
    void init_file() {
        file_header.magic_number = 0x42505442;
        file_header.root_offset = 0;
        file_header.next_free_offset = BLOCK_SIZE;
        save_file_header();
        
        size_t root_offset = allocate_block();
        LeafNode root;
        root.header.self_offset = root_offset;
        file_header.root_offset = root_offset;
        save_file_header();
        write_leaf_node(root_offset, root);
    }
    
    void write_internal_node(size_t offset, const InternalNode& node) {
        index_file.seekp(offset, ios::beg);
        index_file.write(reinterpret_cast<const char*>(&node), sizeof(InternalNode));
        index_file.flush();
    }
    
    void write_leaf_node(size_t offset, const LeafNode& node) {
        index_file.seekp(offset, ios::beg);
        index_file.write(reinterpret_cast<const char*>(&node), sizeof(LeafNode));
        index_file.flush();
    }
    
    void read_internal_node(size_t offset, InternalNode& node) {
        index_file.seekg(offset, ios::beg);
        index_file.read(reinterpret_cast<char*>(&node), sizeof(InternalNode));
    }
    
    void read_leaf_node(size_t offset, LeafNode& node) {
        index_file.seekg(offset, ios::beg);
        index_file.read(reinterpret_cast<char*>(&node), sizeof(LeafNode));
    }
    
    bool is_leaf(size_t offset) {
        if (offset == 0) return false;
        NodeHeader header;
        index_file.seekg(offset, ios::beg);
        index_file.read(reinterpret_cast<char*>(&header), sizeof(NodeHeader));
        return header.type == LEAF;
    }
    
    size_t find_leaf(const IndexKey& key) {
        size_t current = file_header.root_offset;
        if (current == 0) return 0;
        
        while (!is_leaf(current)) {
            InternalNode node;
            read_internal_node(current, node);
            
            int idx = 0;
            for (idx = 0; idx < node.header.key_count; idx++) {
                if (key < node.keys[idx]) break;
            }
            current = node.children[idx];
        }
        return current;
    }
    
    bool insert_into_leaf(LeafNode& leaf, const IndexKey& key) {
        int i = 0;
        while (i < leaf.header.key_count && leaf.keys[i] < key) {
            i++;
        }
        
        if (i < leaf.header.key_count && leaf.keys[i] == key) {
            return false;
        }
        
        for (int j = leaf.header.key_count; j > i; j--) {
            leaf.keys[j] = leaf.keys[j - 1];
        }
        
        leaf.keys[i] = key;
        leaf.header.key_count++;
        return true;
    }
    
    pair<size_t, IndexKey> split_leaf(LeafNode& leaf, size_t leaf_offset) {
        size_t new_offset = allocate_block();
        LeafNode new_leaf;
        new_leaf.header.type = LEAF;
        new_leaf.header.self_offset = new_offset;
        new_leaf.header.parent_offset = leaf.header.parent_offset;
        
        int mid = leaf.header.key_count / 2;
        
        for (int i = mid; i < leaf.header.key_count; i++) {
            new_leaf.keys[i - mid] = leaf.keys[i];
        }
        new_leaf.header.key_count = leaf.header.key_count - mid;
        leaf.header.key_count = mid;
        
        new_leaf.next_leaf = leaf.next_leaf;
        new_leaf.prev_leaf = leaf_offset;
        leaf.next_leaf = new_offset;
        
        write_leaf_node(leaf_offset, leaf);
        write_leaf_node(new_offset, new_leaf);
        
        return {new_offset, new_leaf.keys[0]};
    }
    
    void insert_into_internal(size_t node_offset, const IndexKey& key, size_t right_child) {
        if (node_offset == 0) {
            size_t new_root_offset = allocate_block();
            InternalNode new_root;
            new_root.header.type = INTERNAL;
            new_root.header.self_offset = new_root_offset;
            new_root.header.parent_offset = 0;
            new_root.keys[0] = key;
            new_root.children[0] = file_header.root_offset;
            new_root.children[1] = right_child;
            new_root.header.key_count = 1;
            
            if (is_leaf(new_root.children[0])) {
                LeafNode left_child;
                read_leaf_node(new_root.children[0], left_child);
                left_child.header.parent_offset = new_root_offset;
                write_leaf_node(new_root.children[0], left_child);
            } else {
                InternalNode left_child;
                read_internal_node(new_root.children[0], left_child);
                left_child.header.parent_offset = new_root_offset;
                write_internal_node(new_root.children[0], left_child);
            }
            
            if (is_leaf(new_root.children[1])) {
                LeafNode right_child_node;
                read_leaf_node(new_root.children[1], right_child_node);
                right_child_node.header.parent_offset = new_root_offset;
                write_leaf_node(new_root.children[1], right_child_node);
            } else {
                InternalNode right_child_node;
                read_internal_node(new_root.children[1], right_child_node);
                right_child_node.header.parent_offset = new_root_offset;
                write_internal_node(new_root.children[1], right_child_node);
            }
            
            file_header.root_offset = new_root_offset;
            save_file_header();
            write_internal_node(new_root_offset, new_root);
            return;
        }
        
        InternalNode node;
        read_internal_node(node_offset, node);
        
        int i = 0;
        for (i = 0; i < node.header.key_count; i++) {
            if (key < node.keys[i]) break;
        }
        
        for (int j = node.header.key_count; j > i; j--) {
            node.keys[j] = node.keys[j - 1];
            node.children[j + 1] = node.children[j];
        }
        
        node.keys[i] = key;
        node.children[i + 1] = right_child;
        node.header.key_count++;
        
        if (node.header.key_count <= MAX_KEYS_PER_NODE) {
            write_internal_node(node_offset, node);
        } else {
            size_t new_internal_offset = allocate_block();
            InternalNode new_internal;
            new_internal.header.type = INTERNAL;
            new_internal.header.self_offset = new_internal_offset;
            new_internal.header.parent_offset = node.header.parent_offset;
            
            int mid = node.header.key_count / 2;
            IndexKey mid_key = node.keys[mid];
            
            for (int j = mid + 1; j < node.header.key_count; j++) {
                new_internal.keys[j - mid - 1] = node.keys[j];
            }
            for (int j = mid + 1; j <= node.header.key_count; j++) {
                new_internal.children[j - mid - 1] = node.children[j];
            }
            new_internal.header.key_count = node.header.key_count - mid - 1;
            node.header.key_count = mid;
            
            write_internal_node(node_offset, node);
            write_internal_node(new_internal_offset, new_internal);
            
            for (int j = 0; j <= new_internal.header.key_count; j++) {
                if (is_leaf(new_internal.children[j])) {
                    LeafNode child;
                    read_leaf_node(new_internal.children[j], child);
                    child.header.parent_offset = new_internal_offset;
                    write_leaf_node(new_internal.children[j], child);
                } else {
                    InternalNode child;
                    read_internal_node(new_internal.children[j], child);
                    child.header.parent_offset = new_internal_offset;
                    write_internal_node(new_internal.children[j], child);
                }
            }
            
            insert_into_internal(node.header.parent_offset, mid_key, new_internal_offset);
        }
    }
    
    bool delete_from_leaf(LeafNode& leaf, const IndexKey& key) {
        int i = 0;
        while (i < leaf.header.key_count && !(leaf.keys[i] == key)) {
            i++;
        }
        
        if (i == leaf.header.key_count) {
            return false;
        }
        
        for (int j = i; j < leaf.header.key_count - 1; j++) {
            leaf.keys[j] = leaf.keys[j + 1];
        }
        leaf.header.key_count--;
        return true;
    }

public:
    BPlusTree(const string& fname) : filename(fname) {
        index_file.open(filename, ios::in | ios::out | ios::binary);
        if (!index_file.is_open()) {
            index_file.open(filename, ios::out | ios::binary);
            index_file.close();
            index_file.open(filename, ios::in | ios::out | ios::binary);
            init_file();
        } else {
            load_file_header();
            if (file_header.magic_number != 0x42505442) {
                init_file();
            }
        }
    }
    
    ~BPlusTree() {
        index_file.close();
    }
    
    void insert(const string& key, int value) {
        IndexKey idx_key(key, value);
        size_t leaf_offset = find_leaf(idx_key);
        
        if (leaf_offset == 0) {
            size_t root_offset = allocate_block();
            LeafNode root;
            root.header.self_offset = root_offset;
            root.keys[0] = idx_key;
            root.header.key_count = 1;
            file_header.root_offset = root_offset;
            save_file_header();
            write_leaf_node(root_offset, root);
            return;
        }
        
        LeafNode leaf;
        read_leaf_node(leaf_offset, leaf);
        
        for (int i = 0; i < leaf.header.key_count; i++) {
            if (leaf.keys[i] == idx_key) {
                return;
            }
        }
        
        if (leaf.header.key_count < MAX_KEYS_PER_NODE) {
            insert_into_leaf(leaf, idx_key);
            write_leaf_node(leaf_offset, leaf);
        } else {
            insert_into_leaf(leaf, idx_key);
            auto result = split_leaf(leaf, leaf_offset);
            insert_into_internal(leaf.header.parent_offset, result.second, result.first);
        }
    }
    
    void remove(const string& key, int value) {
        IndexKey idx_key(key, value);
        size_t leaf_offset = find_leaf(idx_key);
        
        if (leaf_offset == 0) return;
        
        LeafNode leaf;
        read_leaf_node(leaf_offset, leaf);
        
        int original_count = leaf.header.key_count;
        delete_from_leaf(leaf, idx_key);
        
        if (leaf.header.key_count == original_count) {
            return;
        }
        
        write_leaf_node(leaf_offset, leaf);
    }
    
    vector<int> find(const string& key) {
        vector<int> values;
        IndexKey search_key(key, INT_MIN);
        
        size_t leaf_offset = find_leaf(search_key);
        if (leaf_offset == 0) return values;
        
        LeafNode leaf;
        read_leaf_node(leaf_offset, leaf);
        
        bool found = false;
        while (leaf_offset != 0) {
            for (int i = 0; i < leaf.header.key_count; i++) {
                if (strcmp(leaf.keys[i].key, key.c_str()) == 0) {
                    values.push_back(leaf.keys[i].value);
                    found = true;
                } else if (found) {
                    return values;
                }
            }
            
            if (leaf.next_leaf == 0) break;
            leaf_offset = leaf.next_leaf;
            read_leaf_node(leaf_offset, leaf);
        }
        
        return values;
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    
    BPlusTree tree("data.db");
    
    int n;
    cin >> n;
    
    while (n--) {
        string cmd;
        cin >> cmd;
        
        if (cmd == "insert") {
            string key;
            int value;
            cin >> key >> value;
            tree.insert(key, value);
        } else if (cmd == "delete") {
            string key;
            int value;
            cin >> key >> value;
            tree.remove(key, value);
        } else if (cmd == "find") {
            string key;
            cin >> key;
            vector<int> values = tree.find(key);
            if (values.empty()) {
                cout << "null" << endl;
            } else {
                for (size_t i = 0; i < values.size(); i++) {
                    if (i > 0) cout << " ";
                    cout << values[i];
                }
                cout << endl;
            }
        }
    }
    
    return 0;
}

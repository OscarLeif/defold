(ns editor.properties
  (:require [clojure.set :as set]
            [camel-snake-kebab :as camel]
            [dynamo.graph :as g]))

(defn- property-edit-type [property]
  (or (get property :edit-type)
      {:type (g/property-value-type (:type property))}))

(def ^:private links #{:link :override})

(defn- flatten-properties [properties]
  (let [pairs (seq properties)
        flat-pairs (filter #(not-any? links (keys (second %))) pairs)
        link-pairs (filter #(contains? (second %) :link) pairs)
        override-pairs (filter #(contains? (second %) :override) pairs)]
    (reduce merge (into {} flat-pairs) (concat
                                         (map #(flatten-properties (:link (second %))) link-pairs)
                                         (mapcat (fn [[k v]]
                                                   (let [k (if (vector? k) k (vector k))]
                                                     (map (fn [[o-k o-v]]
                                                            (let [prop (-> o-v
                                                                         (set/rename-keys {:value :default-value})
                                                                         (assoc :node-id (:node-id v)
                                                                                :value (get (:value v) o-k)))]
                                                              [(conj k o-k) prop]))
                                                         (flatten-properties (:override v)))))
                                              override-pairs)))))

(defn coalesce [properties]
  (let [properties (mapv flatten-properties properties)
        node-count (count properties)
        ; Filter out invisible properties
        ; TODO - not= k :id is a hack since intrinsics are currently included in :properties output
        visible-props (mapcat (fn [p] (filter (fn [[k v]] (and (not= k :_id) (get v :visible true))) p)) properties)
        ; Filter out properties not common to *all* property sets
        ; Heuristic is to compare count and also type
        common-props (filter (fn [[k v]] (and (= node-count (count v)) (apply = (map property-edit-type v))))
                             (map (fn [[k v]] [k (mapv second v)]) (group-by first visible-props)))
        ; Coalesce into properties consumable by e.g. the properties view
        coalesced (into {} (map (fn [[k v]]
                                  (let [prop {:key k
                                              :node-ids (mapv :node-id v)
                                              :values (mapv :value v)
                                              :edit-type (property-edit-type (first v))}
                                        default-vals (mapv :default-value (filter #(contains? % :default-value) v))
                                        prop (if (empty? default-vals) prop (assoc prop :default-values default-vals))]
                                    [k prop]))
                                common-props))]
    coalesced))

(defn values [property]
  (if (contains? property :default-values)
    (mapv (fn [v d-v] (if (nil? v) d-v v)) (:values property) (:default-values property))
    (:values property)))

(defn- set-values [property values]
  (let [key (:key property)]
    (for [[node-id value] (map vector (:node-ids property) values)
          :let [node (g/node-by-id node-id)]]
      (if (vector? key)
        (g/update-property node (first key) assoc-in (rest key) value)
        (g/set-property node (:key property) value)))))

(defn label
  [property]
  (let [k (:key property)
        k (if (vector? k) (last k) k)]
    (-> k
      name
      camel/->Camel_Snake_Case_String
      (clojure.string/replace "_" " "))))

(defn set-values! [property values]
  (g/transact
    (concat
      (g/operation-label (str "Set " (label property)))
      (set-values property values))))

(defn- dissoc-in
  "Dissociates an entry from a nested associative structure returning a new
  nested structure. keys is a sequence of keys. Any empty maps that result
  will not be present in the new structure."
  [m [k & ks :as keys]]
  (if ks
    (if-let [nextmap (get m k)]
      (let [newmap (dissoc-in nextmap ks)]
        (if (empty? newmap)
          (dissoc m k)
          (assoc m k newmap)))
      m)
    (dissoc m k)))

(defn unify-values [values]
  (loop [v0 (first values)
         values (rest values)]
    (if (not (empty? values))
      (let [v (first values)]
        (if (= v0 v)
          (recur v0 (rest values))
          nil))
      v0)))

(defn overridden? [property]
  (and (contains? property :default-values) (not-every? nil? (:values property))))

(defn clear-override! [property]
  (when (overridden? property)
    (g/transact
      (concat
        (g/operation-label (str "Set " (label property)))
        (let [key (:key property)]
          (for [node-id (:node-ids property)
                :let [node (g/node-by-id node-id)]]
            (g/update-property node (first key) dissoc-in (rest key))))))))

package com.dynamo.cr.web2.client.place;

import com.google.gwt.place.shared.Place;
import com.google.gwt.place.shared.PlaceTokenizer;
import com.google.gwt.place.shared.Prefix;

public class DashboardPlace extends Place {
    @Prefix("dashboard")
    public static class Tokenizer implements PlaceTokenizer<DashboardPlace> {
        @Override
        public String getToken(DashboardPlace place) {
            return "";
        }

        @Override
        public DashboardPlace getPlace(String token) {
            return new DashboardPlace();
        }
    }
}

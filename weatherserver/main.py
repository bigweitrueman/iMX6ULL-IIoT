from flask import Flask, jsonify
import requests
import time

app = Flask(__name__)

API_KEY = "ee90ab4c43a09252de97986f1c6bfc36"
CITY = "Beijing"
API_URL = "http://api.openweathermap.org/data/2.5/weather"

cache = {"data": None, "timestamp": 0}

def get_real_weather():
    if cache["data"] and (time.time() - cache["timestamp"] < 600):
        print("Using cached weather data")
        return cache["data"]

    try:
        print("Fetching new data from OpenWeatherMap...")
        params = {"q": CITY, "appid": API_KEY, "units": "metric"}
        resp = requests.get(API_URL, params=params, timeout=5)
        raw = resp.json()

        if resp.status_code != 200:
            print(f"API Error: {resp.status_code} {raw}")
            return {"city": "错误", "weather": "N/A", "temp": 0}

        data = {
            "city": raw["name"],
            "weather": raw["weather"][0]["main"],
            "temp": int(raw["main"]["temp"]),
        }
        cache["data"] = data
        cache["timestamp"] = time.time()
        return data
    except Exception as e:
        print(f"Error: {e}")
        return {"city": "异常", "weather": "N/A", "temp": 0}

@app.route("/weather.json", methods=["GET"])
def weather_endpoint():
    data = get_real_weather()
    print(f"Sending to board: {data}")
    return jsonify(data)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080)
# Weather Server

This project is designed to fetch real-time weather data from a weather API and send it to a development board. Below are the instructions on how to set up and run the project.

## Project Structure

```
weatherserver
├── main.py
├── config.json
├── requirements.txt
└── README.md
```

## Setup Instructions

1. **Clone the Repository**
   Clone this repository to your local machine.

2. **Install Dependencies**
   Navigate to the project directory and install the required Python libraries using pip. You can do this by running the following command:

   ```
   pip install -r requirements.txt
   ```

3. **Configure the API Settings**
   Open the `config.json` file and enter your weather API URL and key. The structure of the `config.json` file should look like this:

   ```json
   {
       "api_url": "YOUR_WEATHER_API_URL",
       "api_key": "YOUR_API_KEY"
   }
   ```

   Replace `YOUR_WEATHER_API_URL` and `YOUR_API_KEY` with the actual values provided by your weather API service.

4. **Run the Application**
   To start the application, run the following command:

   ```
   python main.py
   ```

   This will initiate the process of fetching weather data and sending it to the development board.

## Functionality

- The application reads the API configuration from `config.json`.
- It uses the `requests` library to send HTTP requests to the weather API.
- The API response is processed to extract relevant weather data.
- The fetched weather data is then sent to the development board for further use.

## Notes

Ensure that you have a stable internet connection while running the application, as it requires access to the weather API.